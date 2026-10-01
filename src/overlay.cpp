// In-game dev menu: Dear ImGui drawn into the image the game is about to present.
//
// The menu is recorded into our own command buffer and submitted on the game's queue from inside
// its vkQueuePresentKHR call: that submit waits on the game's present semaphores, and the present
// then waits on ours. DLSS-G sees the menu as UI (it is not in the HUD-less image). While the menu
// is hidden nothing is created, recorded or submitted.
//
// Input: the game window's WndProc is subclassed. While the menu is open, mouse input over the
// menu goes to ImGui and not to the game; everything else passes through.
#include "overlay.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include <backends/imgui_impl_vulkan.h>
#include <backends/imgui_impl_win32.h>
#include <imgui.h>

#include "config.h"
#include "frame.h"
#include "hooks.h"
#include "log.h"
#include "feverscaler_version.h"
#include "sl_bridge.h"
#include "vram.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace feverscaler {
namespace {
constexpr uint32_t kRing = 4;  // command buffers in flight (also ImGui's vertex buffer ring)
struct RingEntry {
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
};

std::atomic<bool> g_visible{false};
// ImGui state is shared by the window thread (WndProc) and the present thread. Recursive: the
// Win32 backend calls ReleaseCapture/SetCapture, which send WM_CAPTURECHANGED straight back into
// the WndProc on the same thread.
std::recursive_mutex g_imguiMx;
bool g_failed = false;
bool g_ctx = false;     // ImGui context + Win32 backend + WndProc subclass
bool g_vkInit = false;  // ImGui Vulkan backend + our pool / ring / render pass
HWND g_hwnd = nullptr;
WNDPROC g_origProc = nullptr;

VkCommandPool g_pool = VK_NULL_HANDLE;
RingEntry g_ring[kRing];
uint32_t g_ringNext = 0;
VkRenderPass g_rp = VK_NULL_HANDLE;
VkFormat g_rpFormat = VK_FORMAT_UNDEFINED;

// The game's swapchain (the images it renders to; with DLSS-G these are Streamline's proxies).
VkSwapchainKHR g_sc = VK_NULL_HANDLE;
VkFormat g_scFormat = VK_FORMAT_UNDEFINED;
VkExtent2D g_scExtent{};
std::vector<VkImage> g_images;
std::vector<VkImageView> g_views;
std::vector<VkFramebuffer> g_fbs;
// One per image index, reused across swapchains: an index is only signalled again after the game
// has re-acquired that image, i.e. after the present that waited on it.
std::vector<VkSemaphore> g_sems;

void CheckVk(VkResult r) {
  if (r != VK_SUCCESS) FEVERSCALER_LOG_N(10, "overlay: Vulkan error %d", (int)r);
}

// ---- window + input ------------------------------------------------------------------------------
BOOL CALLBACK FindGameWindow(HWND h, LPARAM out) {
  DWORD pid = 0;
  GetWindowThreadProcessId(h, &pid);
  if (pid != GetCurrentProcessId() || !IsWindowVisible(h)) return TRUE;
  char cls[64]{};
  GetClassNameA(h, cls, sizeof(cls));
  if (strcmp(cls, "SDL_app") != 0) return TRUE;
  *(HWND*)out = h;
  return FALSE;
}

constexpr UINT kMsgRun = WM_APP + 0x3F1;  // wParam: void(*)() to run on the window thread

LRESULT CALLBACK OverlayWndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
  if (msg == kMsgRun && w) {
    try {
      ((void (*)())w)();
    } catch (...) {
      Log("window thread task threw");
    }
    return 0;
  }
  if (g_visible.load(std::memory_order_relaxed) && g_ctx) {
    bool captureMouse = false;
    try {  // nothing of ours may unwind into the game's message loop
      std::lock_guard<std::recursive_mutex> lk(g_imguiMx);
      ImGui_ImplWin32_WndProcHandler(h, msg, w, l);
      captureMouse = ImGui::GetIO().WantCaptureMouse;
    } catch (...) {
      FEVERSCALER_LOG_N(5, "overlay: exception in WndProc (msg 0x%X)", msg);
    }
    bool mouseMsg = (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || msg == WM_INPUT;
    if (captureMouse && mouseMsg) return msg == WM_INPUT ? DefWindowProcW(h, msg, w, l) : 0;
  }
  return CallWindowProcW(g_origProc, h, msg, w, l);
}

// Subclasses the game window once (menu input, and tasks that must run on the window thread).
std::mutex g_winMx;
bool EnsureWindowHook() {
  std::lock_guard<std::mutex> lk(g_winMx);
  if (g_origProc) return true;
  HWND hwnd = nullptr;
  EnumWindows(FindGameWindow, (LPARAM)&hwnd);
  if (!hwnd) return false;
  g_hwnd = hwnd;
  g_origProc = (WNDPROC)GetWindowLongPtrW(g_hwnd, GWLP_WNDPROC);
  SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)OverlayWndProc);
  Log("game window %p hooked (thread %lu)", (void*)g_hwnd, GetWindowThreadProcessId(g_hwnd, nullptr));
  return true;
}

bool InitContext() {
  if (!EnsureWindowHook()) {
    Log("overlay: game window not found");
    return false;
  }
  std::lock_guard<std::recursive_mutex> lk(g_imguiMx);
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.LogFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;  // the game owns the cursor shape

  // Sized for the output resolution: ~22 px text at 1440p.
  float px = std::round((float)g_scExtent.height / 64.0f);
  px = px < 15.f ? 15.f : (px > 36.f ? 36.f : px);
  const char* segoe = "C:\\Windows\\Fonts\\segoeui.ttf";
  if (GetFileAttributesA(segoe) == INVALID_FILE_ATTRIBUTES || !io.Fonts->AddFontFromFileTTF(segoe, px))
    io.Fonts->AddFontDefault();
  ImGui::StyleColorsDark();
  ImGuiStyle& st = ImGui::GetStyle();
  st.ScaleAllSizes(px / 13.0f);
  st.FontSizeBase = px;
  st.WindowRounding = 6.0f * px / 13.0f;
  st.FrameRounding = 4.0f * px / 13.0f;
  st.Colors[ImGuiCol_WindowBg].w = 0.92f;

  ImGui_ImplWin32_Init(g_hwnd);
  // No IME handling: it would re-associate the game window's input context from our thread.
  ImGui::GetPlatformIO().Platform_SetImeDataFn = nullptr;
  g_ctx = true;
  Log("overlay: ImGui %s on window %p, font %.0f px", IMGUI_VERSION, (void*)g_hwnd, px);
  return true;
}

// ---- Vulkan ----------------------------------------------------------------------------------------
VkRenderPass CreateRenderPass(VkFormat format) {
  VkAttachmentDescription a{};
  a.format = format;
  a.samples = VK_SAMPLE_COUNT_1_BIT;
  a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;  // draw over the finished frame
  a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  a.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  a.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sub{};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sub.colorAttachmentCount = 1;
  sub.pColorAttachments = &ref;
  VkSubpassDependency dep[2]{};
  dep[0].srcSubpass = VK_SUBPASS_EXTERNAL;  // the game's writes to the image (same queue, or semaphore)
  dep[0].dstSubpass = 0;
  dep[0].srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
  dep[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
  dep[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  dep[1].srcSubpass = 0;
  dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
  dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  dep[1].dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
  dep[1].dstAccessMask = 0;
  VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  ci.attachmentCount = 1;
  ci.pAttachments = &a;
  ci.subpassCount = 1;
  ci.pSubpasses = &sub;
  ci.dependencyCount = 2;
  ci.pDependencies = dep;
  VkRenderPass rp = VK_NULL_HANDLE;
  if (vk.vkCreateRenderPass(gDevice, &ci, nullptr, &rp) != VK_SUCCESS) return VK_NULL_HANDLE;
  return rp;
}

void WaitRing() {
  for (auto& r : g_ring)
    if (r.fence) vk.vkWaitForFences(gDevice, 1, &r.fence, VK_TRUE, 2'000'000'000ull);
}

bool InitVulkan(VkQueue queue) {
  if (!ImGui_ImplVulkan_LoadFunctions(gApiVersion, [](const char* n, void*) { return RawVkProc(n); })) {
    Log("overlay: ImGui could not load its Vulkan functions");
    return false;
  }
  VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pci.queueFamilyIndex = gQueueFamily;
  if (vk.vkCreateCommandPool(gDevice, &pci, nullptr, &g_pool) != VK_SUCCESS) return false;
  for (auto& r : g_ring) {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = g_pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    if (vk.vkAllocateCommandBuffers(gDevice, &ai, &r.cmd) != VK_SUCCESS) return false;
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (vk.vkCreateFence(gDevice, &fi, nullptr, &r.fence) != VK_SUCCESS) return false;
  }
  g_rp = CreateRenderPass(g_scFormat);
  if (!g_rp) return false;
  g_rpFormat = g_scFormat;

  ImGui_ImplVulkan_InitInfo ii{};
  ii.ApiVersion = gApiVersion;
  ii.Instance = gInstance;
  ii.PhysicalDevice = gPhysicalDevice;
  ii.Device = gDevice;
  ii.QueueFamily = gQueueFamily;
  ii.Queue = queue;  // font texture uploads (submit + wait, once)
  ii.DescriptorPoolSize = IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE;
  ii.MinImageCount = 2;
  ii.ImageCount = kRing;
  ii.PipelineInfoMain.RenderPass = g_rp;
  ii.PipelineInfoMain.Subpass = 0;
  ii.CheckVkResultFn = CheckVk;
  {
    std::lock_guard<std::recursive_mutex> lk(g_imguiMx);
    if (!ImGui_ImplVulkan_Init(&ii)) {
      Log("overlay: ImGui_ImplVulkan_Init failed");
      return false;
    }
  }
  g_vkInit = true;
  Log("overlay: Vulkan backend ready (format %d, %ux%u)", (int)g_scFormat, g_scExtent.width, g_scExtent.height);
  return true;
}

void DestroyTargets() {
  for (VkFramebuffer fb : g_fbs)
    if (fb) vk.vkDestroyFramebuffer(gDevice, fb, nullptr);
  for (VkImageView v : g_views)
    if (v) vk.vkDestroyImageView(gDevice, v, nullptr);
  g_fbs.clear();
  g_views.clear();
}

// Views + framebuffers for the current swapchain images (and a new render pass / pipeline if the
// swapchain format changed).
bool BuildTargets() {
  if (g_images.empty()) return false;
  WaitRing();
  DestroyTargets();
  if (g_rpFormat != g_scFormat) {
    VkRenderPass rp = CreateRenderPass(g_scFormat);
    if (!rp) return false;
    ImGui_ImplVulkan_PipelineInfo pi{};
    pi.RenderPass = rp;
    ImGui_ImplVulkan_CreateMainPipeline(&pi);
    vk.vkDestroyRenderPass(gDevice, g_rp, nullptr);
    g_rp = rp;
    g_rpFormat = g_scFormat;
  }
  g_views.assign(g_images.size(), VK_NULL_HANDLE);
  g_fbs.assign(g_images.size(), VK_NULL_HANDLE);
  for (size_t i = 0; i < g_images.size(); ++i) {
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = g_images[i];
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = g_scFormat;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vk.vkCreateImageView(gDevice, &vi, nullptr, &g_views[i]) != VK_SUCCESS) return false;
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = g_rp;
    fi.attachmentCount = 1;
    fi.pAttachments = &g_views[i];
    fi.width = g_scExtent.width;
    fi.height = g_scExtent.height;
    fi.layers = 1;
    if (vk.vkCreateFramebuffer(gDevice, &fi, nullptr, &g_fbs[i]) != VK_SUCCESS) return false;
  }
  while (g_sems.size() < g_images.size()) {
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore s = VK_NULL_HANDLE;
    if (vk.vkCreateSemaphore(gDevice, &si, nullptr, &s) != VK_SUCCESS) return false;
    g_sems.push_back(s);
  }
  return true;
}

// ---- the menu ----------------------------------------------------------------------------------------
// A line that wraps instead of widening the auto-sized window.
void Note(const ImVec4& color, const char* label, const char* text) {
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetFontSize() * 30.0f);
  ImGui::TextColored(color, "%s%s", label, text);
  ImGui::PopTextWrapPos();
}

void DrawMenu() {
  const Config& cfg = Cfg();
  FgStatus fg = FgGetStatus();
  SlFgState sl = SlGetFgState();
  ImGuiIO& io = ImGui::GetIO();
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.03f, io.DisplaySize.y * 0.10f), ImGuiCond_FirstUseEver);
  bool open = true;
  ImGui::Begin("FeverScaler", &open,
               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
  ImGui::TextDisabled("v" FEVERSCALER_VERSION);
  const ImVec4 green(0.45f, 0.85f, 0.45f, 1), amber(0.95f, 0.75f, 0.30f, 1), red(0.95f, 0.40f, 0.35f, 1);

  ImGui::BeginDisabled(!fg.available);
  bool on = fg.userEnabled;
  if (ImGui::Checkbox("DLSS Frame Generation", &on)) FgSetUserEnabled(on);

  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Multiplier");
  ImGui::BeginDisabled(!fg.userEnabled);
  for (uint32_t n = 1; n <= kMaxGeneratedFrames; ++n) {
    char label[8];
    snprintf(label, sizeof(label), "x%u", n + 1);
    ImGui::SameLine();
    ImGui::BeginDisabled(n > fg.maxGeneratedFrames);
    if (ImGui::RadioButton(label, fg.generatedFrames == n)) FgSetGeneratedFrames(n);
    ImGui::EndDisabled();
  }
  ImGui::EndDisabled();
  if (fg.available && fg.maxGeneratedFrames < kMaxGeneratedFrames)
    ImGui::TextDisabled("DLSS-G on this GPU/driver allows up to x%u", fg.maxGeneratedFrames + 1);

  ImGui::EndDisabled();  // !fg.available

  ImGui::Separator();
  if (!fg.available) {
    Note(red, "Frame generation unavailable: ", fg.unavailableReason);
  } else if (!fg.userEnabled) {
    ImGui::TextDisabled("Off");
  } else if (sl.optionsError) {
    ImGui::TextColored(red, "DLSS-G could not apply settings (%d); see feverscaler-events.log", sl.optionsError);
  } else if (sl.status) {
    const char* why = (sl.status & 1)   ? "output resolution too low"
                      : (sl.status & 2) ? "Reflex not active"
                      : (sl.status & 4) ? "HDR format not supported"
                      : (sl.status & 8) ? "invalid camera constants"
                                        : "see feverscaler-events.log";
    ImGui::TextColored(red, "DLSS-G error 0x%X: %s", sl.status, why);
  } else if (fg.active) {
    ImGui::TextColored(green, "Active: x%u", fg.generatedFrames + 1);
  } else if (!fg.have3D) {
    ImGui::TextColored(amber, "Waiting for the 3D view (menu or loading screen)");
  } else {
    ImGui::TextColored(amber, "Starting...");
  }
  ImGui::Text("Rendered %.0f fps, shown %.0f fps (x%.2f)", fg.baseFps, fg.baseFps * sl.ratio, sl.ratio);
  VramInfo vr = QueryVram();
  if (vr.valid) {
    const double gb = 1024.0 * 1024.0 * 1024.0;
    bool over = sl.overBudget || vr.usage > vr.budget;
    ImGui::TextColored(over ? red : ImVec4(0.8f, 0.8f, 0.8f, 1), "VRAM %.1f / %.1f GB%s (DLSS-G %.2f GB, DLSS %.2f GB)",
                       vr.usage / gb, vr.budget / gb, over ? " - OVER BUDGET, expect stutter" : "", sl.fgVram / gb,
                       sl.srVram / gb);
  }

  ImGui::Separator();
  SrStatus sr = SrGetStatus();
  ImGui::BeginDisabled(!sr.available);
  bool srOn = sr.wanted;
  if (ImGui::Checkbox("DLSS Super Resolution", &srOn)) SrSetEnabled(srOn);
  ImGui::EndDisabled();
  if (!sr.available) {
    Note(red, "DLSS unavailable: ", sr.unavailableReason);
  } else if (sr.wanted) {
    if (sr.active)
      ImGui::TextColored(green, "%s: %ux%u -> %ux%u", sr.mode, sr.renderW, sr.renderH, sr.outW, sr.outH);
    else
      ImGui::TextColored(amber, "Waiting for the 3D view");
    if (sr.scaleControl) {
      ImGui::TextUnformatted("Render resolution");
      for (uint32_t m = 1; m <= kDlssModeLast; ++m) {
        char label[64];
        float sc = DlssModeScale(m);
        snprintf(label, sizeof(label), "%s (%.0f%%)", DlssModeName(m), sc * 100.0f);
        if (m != 1 && m != 4) ImGui::SameLine();
        if (ImGui::RadioButton(label, sr.renderMode == m)) SrSetMode(m);
      }
      // Vehicle / station window previews: DLSS does not upscale them.
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted("Previews");
      static const char* kPreview[kPreviewScaleCount] = {"Full resolution", "Preset scale"};
      for (uint32_t m = 0; m < kPreviewScaleCount; ++m) {
        ImGui::SameLine();
        if (ImGui::RadioButton(kPreview[m], cfg.previewScale == m)) SavePreviewScale(m);
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Render size of the 3D preview in vehicle / station windows (not upscaled by DLSS).\n"
                            "Applies the next time a preview window opens.");
      }
    } else {
      ImGui::TextDisabled("Render size = the game's Resolution Scale (this game build: no preset control)");
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Model");
    struct Opt {
      uint32_t preset;
      const char* label;
      const char* tip;
    };
    static const Opt opts[] = {
        {5, "CNN", "DLSS 3 convolutional model (preset E): cheapest, softest"},
        {11, "Transformer", "DLSS 4 transformer model (preset K)"},
        {12, "Transformer 2", "DLSS 4.5 second-generation transformer (preset L): sharpest, most stable, most expensive"},
        {13, "Transformer 2 fast", "DLSS 4.5 second-generation transformer (preset M): close to L, faster"},
    };
    for (const Opt& o : opts) {
      ImGui::SameLine();
      if (ImGui::RadioButton(o.label, sr.preset == o.preset)) SrSetPreset(o.preset);
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", o.tip);
    }
  }

  ImGui::Separator();
  ImGui::TextDisabled("%s: show / hide this menu (change the key in Settings > Graphics)", KeyName(cfg.keyMenu).c_str());
  ImGui::End();
  if (!open) OverlayToggle();
}
}  // namespace

// ---- API -----------------------------------------------------------------------------------------------
bool OverlayRunOnWindowThread(void (*fn)()) {
  if (!EnsureWindowHook()) return false;
  return PostMessageW(g_hwnd, kMsgRun, (WPARAM)fn, 0) != FALSE;
}

DWORD OverlayWindowThread() {
  if (!EnsureWindowHook()) return 0;
  return GetWindowThreadProcessId(g_hwnd, nullptr);
}

void OverlayToggle() {
  bool v = !g_visible.load();
  g_visible = v;
  Log("overlay: menu %s", v ? "shown" : "hidden");
}

void OverlayReleaseSwapchain() {
  if (!g_vkInit) return;
  WaitRing();
  DestroyTargets();
  g_images.clear();
  g_sc = VK_NULL_HANDLE;
}

void OverlayOnSwapchainCreated(VkSwapchainKHR sc, VkFormat format, VkExtent2D extent) {
  g_sc = sc;
  g_scFormat = format;
  g_scExtent = extent;
  g_images.clear();
}

void OverlayOnSwapchainImages(VkSwapchainKHR sc, const VkImage* imgs, uint32_t n) {
  if (sc != g_sc) return;
  g_images.assign(imgs, imgs + n);
  if (g_vkInit) {
    WaitRing();
    DestroyTargets();  // rebuilt on the next visible frame
  }
}

static void PresentImpl(VkQueue queue, VkPresentInfoKHR* pi);

void OverlayPresent(VkQueue queue, VkPresentInfoKHR* pi) {
  if (!g_visible.load() || g_failed || !gDevice) return;
  try {  // nothing of ours may unwind into the game's present call
    PresentImpl(queue, pi);
  } catch (...) {
    Log("overlay: exception while drawing the menu - menu disabled for this session");
    g_failed = true;
  }
}

static void PresentImpl(VkQueue queue, VkPresentInfoKHR* pi) {
  if (!pi || pi->swapchainCount != 1 || pi->pSwapchains[0] != g_sc || g_images.empty()) return;
  uint32_t idx = pi->pImageIndices[0];
  if (idx >= g_images.size()) return;
  if (!g_ctx && !InitContext()) {
    g_failed = true;
    return;
  }
  if (!g_vkInit && !InitVulkan(queue)) {
    g_failed = true;
    return;
  }
  if (g_fbs.size() != g_images.size() && !BuildTargets()) {
    Log("overlay: could not create framebuffers for the swapchain images");
    g_failed = true;
    return;
  }

  ImDrawData* dd = nullptr;
  {
    std::lock_guard<std::recursive_mutex> lk(g_imguiMx);
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGuiIO& io = ImGui::GetIO();
    if (io.DisplaySize.x <= 0 || io.DisplaySize.y <= 0) return;  // minimised
    // Window coordinates -> swapchain pixels (the client area and swapchain can differ slightly).
    io.DisplayFramebufferScale =
        ImVec2((float)g_scExtent.width / io.DisplaySize.x, (float)g_scExtent.height / io.DisplaySize.y);
    // The game may hide the OS cursor and draw its own under our menu: draw one on top then.
    CURSORINFO ci{sizeof(ci)};
    io.MouseDrawCursor = GetCursorInfo(&ci) && !(ci.flags & CURSOR_SHOWING);
    ImGui::NewFrame();
    DrawMenu();
    ImGui::Render();
    dd = ImGui::GetDrawData();
  }

  RingEntry& r = g_ring[g_ringNext];
  if (vk.vkWaitForFences(gDevice, 1, &r.fence, VK_TRUE, 2'000'000'000ull) != VK_SUCCESS) return;
  g_ringNext = (g_ringNext + 1) % kRing;
  vk.vkResetFences(gDevice, 1, &r.fence);
  vk.vkResetCommandBuffer(r.cmd, 0);
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vk.vkBeginCommandBuffer(r.cmd, &bi);
  VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  rb.renderPass = g_rp;
  rb.framebuffer = g_fbs[idx];
  rb.renderArea.extent = g_scExtent;
  vk.vkCmdBeginRenderPass(r.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
  ImGui_ImplVulkan_RenderDrawData(dd, r.cmd);
  vk.vkCmdEndRenderPass(r.cmd);
  vk.vkEndCommandBuffer(r.cmd);

  std::vector<VkPipelineStageFlags> stages(pi->waitSemaphoreCount, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.waitSemaphoreCount = pi->waitSemaphoreCount;
  si.pWaitSemaphores = pi->pWaitSemaphores;
  si.pWaitDstStageMask = stages.data();
  si.commandBufferCount = 1;
  si.pCommandBuffers = &r.cmd;
  si.signalSemaphoreCount = 1;
  si.pSignalSemaphores = &g_sems[idx];
  VkResult sr = vk.vkQueueSubmit(queue, 1, &si, r.fence);
  if (sr != VK_SUCCESS) {
    Log("overlay: submit failed %d - menu disabled for this session", (int)sr);
    vk.vkQueueSubmit(queue, 0, nullptr, r.fence);  // keep the fence usable
    g_failed = true;
    return;  // present unchanged
  }
  pi->waitSemaphoreCount = 1;
  pi->pWaitSemaphores = &g_sems[idx];
  FEVERSCALER_LOG_N(1, "overlay: first menu frame drawn (image %u, %u wait semaphores replaced)", idx, (unsigned)si.waitSemaphoreCount);
}
}  // namespace feverscaler
