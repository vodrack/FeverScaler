// Vulkan interception, following bg3fgvk's proven model (MIT, github.com/thierbig/bg3fgvk):
// vulkan-1.dll's exported vkGetInstanceProcAddr is detoured in place. TF3 (through SDL2) resolves
// every Vulkan function through it, so the game ends up on the Streamline interposer's
// functions - the interposer performs the device "surgery" (extra queues/extensions) and runs
// DLSS-G inside its vkQueuePresentKHR. Our own wrappers sit on top for the functions we track.
#include "hooks.h"

#include <windows.h>

#include <MinHook.h>

#include <atomic>
#include <cstring>

#include "config.h"
#include "diagnostics.h"
#include "log.h"
#include "sl_bridge.h"

namespace feverscaler {
VkNext vk;
VkInstance gInstance{};
uint32_t gApiVersion = VK_API_VERSION_1_0;
VkPhysicalDevice gPhysicalDevice{};
VkDevice gDevice{};
uint32_t gQueueFamily = 0;

// Real loader entry (MinHook trampoline) and the interposer's resolvers.
static PFN_vkGetInstanceProcAddr o_GIPA{};
static PFN_vkGetDeviceProcAddr o_GDPA{};  // loader export (not hooked), used for ForceReal lookups
static PFN_vkGetInstanceProcAddr ip_GIPA{};
static PFN_vkGetDeviceProcAddr ip_GDPA{};
static PFN_vkCreateInstance t_CreateInstance{};
static PFN_vkCreateDevice t_CreateDevice{};

// Re-entry guard: the interposer reaches the real loader through the very export we detoured.
// Calls made while g_reentry > 0 originate inside the interposer and must go to the real loader.
static thread_local int g_reentry = 0;
Reentry::Reentry() { ++g_reentry; }
Reentry::~Reentry() { --g_reentry; }
// slInit spawns worker threads that resolve through the export while the game makes no
// Vulkan calls; route every thread to the real loader for that window.
static std::atomic<int> g_globalReal{0};
struct GlobalReal {
  GlobalReal() { ++g_globalReal; }
  ~GlobalReal() { --g_globalReal; }
};
static inline bool ForceReal() { return g_reentry || g_globalReal.load(std::memory_order_acquire); }

static bool g_slOk = false;

static void EnsureSlAndInterposer() {
  static bool inited = false;
  if (inited) return;
  inited = true;
  Reentry _;
  GlobalReal _g;
  ip_GIPA = (PFN_vkGetInstanceProcAddr)SlProxyFn("vkGetInstanceProcAddr");
  ip_GDPA = (PFN_vkGetDeviceProcAddr)SlProxyFn("vkGetDeviceProcAddr");
  g_slOk = ip_GIPA && ip_GDPA && SlInit();
  Log("Streamline: interposer GIPA=%p GDPA=%p slInit=%s", (void*)ip_GIPA, (void*)ip_GDPA, g_slOk ? "ok" : "FAILED");
  if (!g_slOk) { ip_GIPA = nullptr; ip_GDPA = nullptr; }
}

static VKAPI_ATTR VkResult VKAPI_CALL w_CreateInstance(const VkInstanceCreateInfo* ci, const VkAllocationCallbacks* a,
                                                       VkInstance* out) {
  DiagnosticsStart(); // vkCreateInstance is called outside the loader lock
  EnsureSlAndInterposer();
  PFN_vkCreateInstance create = nullptr;
  {
    Reentry _;
    if (ip_GIPA) create = (PFN_vkCreateInstance)ip_GIPA(nullptr, "vkCreateInstance");
    if (!create) create = (PFN_vkCreateInstance)o_GIPA(nullptr, "vkCreateInstance");
  }
  t_CreateInstance = create;
  VkResult r;
  {
    Reentry _;
    r = create(ci, a, out);
  }
  if (r == VK_SUCCESS) {
    gInstance = *out;
    if (ci->pApplicationInfo && ci->pApplicationInfo->apiVersion) gApiVersion = ci->pApplicationInfo->apiVersion;
    Log("vkCreateInstance ok instance=%p apiVersion=0x%x (through %s)", (void*)gInstance,
        ci->pApplicationInfo ? ci->pApplicationInfo->apiVersion : 0, ip_GIPA ? "interposer" : "loader");
  }
  return r;
}

static PFN_vkVoidFunction RawInstanceProc(const char* name) {
  Reentry _;
  PFN_vkVoidFunction f = ip_GIPA ? ip_GIPA(gInstance, name) : nullptr;
  if (!f) f = o_GIPA(gInstance, name);
  return f;
}

PFN_vkVoidFunction RawVkProc(const char* name) {
  PFN_vkVoidFunction f = nullptr;
  if (gDevice) {
    Reentry _;
    f = ip_GDPA ? ip_GDPA(gDevice, name) : nullptr;
    if (!f && o_GDPA) f = o_GDPA(gDevice, name);
  }
  return f ? f : RawInstanceProc(name);
}

static void ResolveDeviceTable(VkDevice dev) {
  Reentry _;
  auto get = [&](const char* name) -> PFN_vkVoidFunction {
    PFN_vkVoidFunction f = ip_GDPA ? ip_GDPA(dev, name) : nullptr;
    if (!f && o_GDPA) f = o_GDPA(dev, name);
    return f;
  };
#define FEVERSCALER_FN(name) vk.name = (PFN_##name)get(#name);
#include "vkfuncs.inl"
#undef FEVERSCALER_FN
  vk.vkGetPhysicalDeviceMemoryProperties =
      (PFN_vkGetPhysicalDeviceMemoryProperties)RawInstanceProc("vkGetPhysicalDeviceMemoryProperties");
  vk.vkGetPhysicalDeviceProperties = (PFN_vkGetPhysicalDeviceProperties)RawInstanceProc("vkGetPhysicalDeviceProperties");
  vk.vkGetPhysicalDeviceFormatProperties =
      (PFN_vkGetPhysicalDeviceFormatProperties)RawInstanceProc("vkGetPhysicalDeviceFormatProperties");
}

static VKAPI_ATTR VkResult VKAPI_CALL w_CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo* ci,
                                                     const VkAllocationCallbacks* a, VkDevice* out) {
  VkResult r;
  {
    Reentry _;
    r = t_CreateDevice(pd, ci, a, out);
  }
  if (r == VK_SUCCESS) {
    gPhysicalDevice = pd;
    gDevice = *out;
    gQueueFamily = ci->queueCreateInfoCount ? ci->pQueueCreateInfos[0].queueFamilyIndex : 0;
    Log("vkCreateDevice ok device=%p queueFamilies=%u family0=%u", (void*)gDevice, ci->queueCreateInfoCount,
        ci->queueCreateInfoCount ? ci->pQueueCreateInfos[0].queueFamilyIndex : 0);
    ResolveDeviceTable(gDevice);
    if (vk.vkGetPhysicalDeviceProperties) {
      VkPhysicalDeviceProperties properties{};
      vk.vkGetPhysicalDeviceProperties(pd, &properties);
      char system[192];
      DiagnosticsSystem(system, sizeof(system));
      LogEnvironment("%s GPU=%s vendor=0x%x device=0x%x driverRaw=0x%x Vulkan=0x%x queueFamily=%u", system,
          properties.deviceName, properties.vendorID, properties.deviceID, properties.driverVersion,
          properties.apiVersion, gQueueFamily);
    }
    if (g_slOk) {
      Reentry _;
      SlOnDeviceCreated(pd);
    }
  }
  return r;
}

// ---- device proc addr: interposer function, wrapped where we track --------------------------
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL w_GetDeviceProcAddr(VkDevice dev, const char* name);

static PFN_vkVoidFunction GameViewGDPA(VkDevice dev, const char* name) {
  if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)w_GetDeviceProcAddr;
  PFN_vkVoidFunction ip;
  {
    Reentry _;
    ip = ip_GDPA ? ip_GDPA(dev, name) : (o_GDPA ? o_GDPA(dev, name) : nullptr);
  }
  if (!ip) return nullptr;
  PFN_vkVoidFunction w = LookupDeviceWrapper(name, ip);
  return w ? w : ip;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL w_GetDeviceProcAddr(VkDevice dev, const char* name) {
  if (!name) return nullptr;
  if (ForceReal()) return o_GDPA ? o_GDPA(dev, name) : nullptr;
  return GameViewGDPA(dev, name);
}

// ---- the single entry hook ----------------------------------------------------------------------
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL h_GetInstanceProcAddr(VkInstance inst, const char* name) {
  if (!name) return nullptr;
  if (ForceReal()) return o_GIPA(inst, name);
  // slInit happens in the vkCreateInstance CALL, not here: this resolve can run under the
  // loader lock, and slInit's plugin loading needs that lock (deadlock otherwise).
  if (!strcmp(name, "vkCreateInstance")) return (PFN_vkVoidFunction)w_CreateInstance;
  PFN_vkVoidFunction ip = nullptr;
  if (ip_GIPA) {
    Reentry _;
    ip = ip_GIPA(inst, name);
  }
  if (!ip) ip = o_GIPA(inst, name);
  if (!ip) return nullptr;
  if (!strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)h_GetInstanceProcAddr;
  if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)w_GetDeviceProcAddr;
  if (!strcmp(name, "vkCreateDevice")) {
    t_CreateDevice = (PFN_vkCreateDevice)ip;
    return (PFN_vkVoidFunction)w_CreateDevice;
  }
  PFN_vkVoidFunction w = LookupDeviceWrapper(name, ip);
  return w ? w : ip;
}

static void InstallVkHooks(HMODULE vulkan) {
  static std::atomic<bool> done{false};
  bool exp = false;
  if (!done.compare_exchange_strong(exp, true)) return;
  void* gipa = (void*)GetProcAddress(vulkan, "vkGetInstanceProcAddr");
  o_GDPA = (PFN_vkGetDeviceProcAddr)GetProcAddress(vulkan, "vkGetDeviceProcAddr");
  if (!gipa || !o_GDPA) {
    Log("InstallVkHooks: vulkan-1 exports missing");
    return;
  }
  MH_STATUS s1 = MH_CreateHook(gipa, (void*)h_GetInstanceProcAddr, (void**)&o_GIPA);
  MH_STATUS s2 = MH_EnableHook(gipa);
  Log("InstallVkHooks: vkGetInstanceProcAddr hooked (%d/%d)", (int)s1, (int)s2);
}

// ---- LoadLibraryExW: catch vulkan-1.dll ---------------------------------------------------------
using PFN_LoadLibraryExW = HMODULE(WINAPI*)(LPCWSTR, HANDLE, DWORD);
static PFN_LoadLibraryExW o_LoadLibraryExW{};

static bool IsVulkanLoader(LPCWSTR name) {
  if (!name) return false;
  const wchar_t* base = name;
  for (const wchar_t* p = name; *p; ++p)
    if (*p == L'\\' || *p == L'/') base = p + 1;
  return _wcsnicmp(base, L"vulkan-1", 8) == 0;
}

static HMODULE WINAPI h_LoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags) {
  HMODULE m = o_LoadLibraryExW(name, file, flags);
  if (m && IsVulkanLoader(name)) {
    FEVERSCALER_LOG_N(1, "vulkan-1.dll loaded (%S) - installing Vulkan hooks", name);
    InstallVkHooks(m);
  }
  return m;
}

void InstallLoaderHooks() {
  MH_STATUS s = MH_Initialize();
  if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) {
    Log("MH_Initialize failed %d", (int)s);
    return;
  }
  if (HMODULE vk1 = GetModuleHandleW(L"vulkan-1.dll")) {
    Log("vulkan-1.dll already loaded - installing Vulkan hooks now");
    InstallVkHooks(vk1);
    return;
  }
  MH_STATUS c = MH_CreateHookApi(L"kernelbase", "LoadLibraryExW", (void*)h_LoadLibraryExW, (void**)&o_LoadLibraryExW);
  MH_STATUS e = MH_EnableHook(MH_ALL_HOOKS);
  Log("LoadLibraryExW hook: create=%d enable=%d", (int)c, (int)e);
}
}  // namespace feverscaler
