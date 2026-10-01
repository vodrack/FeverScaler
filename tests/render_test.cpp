// Real submit/UI/pass-tracking paths with a fake Vulkan dispatch table: no game or GPU.
#include "../src/frame.cpp"
#define g_mx tracker_mx
#include "../src/tracker.cpp"
#undef g_mx
#include "../src/config.cpp"
#include "../src/game_settings.cpp"

#include <cstdio>
#include <cstdlib>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

namespace feverscaler {
VkNext vk;
VkInstance gInstance{};
VkPhysicalDevice gPhysicalDevice{};
VkDevice gDevice = (VkDevice)1;
uint32_t gQueueFamily = 0, gApiVersion = VK_API_VERSION_1_3;
static bool core = true, fgReady = true, srReady = true;
static unsigned gpuInit = 0, renderTargets = 0, submits = 0, tags = 0, hudlessAllocs = 0, srAllocs = 0;
static float slider = 1.0f, overrideScale = 0.0f;
static GpuSlot slot;
static GpuImage srOut;
static bool dlssRuns = false;  // DLSS configures, evaluates and has an output image
static VkPipeline bound{};
static std::wstring noPath;
const std::wstring& PluginDir() { return noPath; }
const std::wstring& GameDir() { return noPath; }
void Log(const char*, ...) {}
void LogEvent(const char*, const char*, ...) {}
void LogFlushRepeats() {}
void BridgePoll() {}
std::wstring BridgeUserDataDir() { return {}; }
void OverlayToggle() {}
void OverlayPresent(VkQueue, VkPresentInfoKHR*) {}
void OverlayReleaseSwapchain() {}
void OverlayOnSwapchainCreated(VkSwapchainKHR, VkFormat, VkExtent2D) {}
void OverlayOnSwapchainImages(VkSwapchainKHR, const VkImage*, uint32_t) {}
bool OverlayRunOnWindowThread(void (*fn)()) { fn(); return true; }
unsigned long OverlayWindowThread() { return GetCurrentThreadId(); }
bool GameScaleAvailable() { return true; }
float GameSliderScale() { return slider; }
float GameScaleOverride() { return overrideScale; }
void GameSetScaleOverride(float s) { overrideScale = s; }
void GameScalePoll() {}
void GameScaleResizeWorld() {}
VramInfo QueryVram() { return {}; }
bool SlCoreReady() { return core; }
bool SlFgReady() { return fgReady; }
bool SlDlssAvailable() { return srReady; }
const char* SlSrUnavailableReason() { return ""; }
const char* SlFgUnavailableReason() { return ""; }
const char* SlDlssModeName() { return "test"; }
void SlOnFirstSubmit() {}
void SlOnPresentBegin() {}
void SlOnPresentEnd() {}
void SlClearTags() {}
void SlPollState() {}
bool SlSetGeneration(bool, uint32_t, bool) { return true; }
uint32_t SlFramesMax() { return 5; }
bool SlTagFrame(const SlImage&, const SlImage&, const SlImage*) { ++tags; return true; }
bool SlSetConstants(const sl::Constants&) { return true; }
bool SlDlssConfigure(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { return dlssRuns; }
bool SlDlssEvaluate(VkCommandBuffer, const SlImage&, const SlImage&, const SlImage&, const SlImage&) { return dlssRuns; }
bool GpuReady() { return true; }
bool GpuInit(uint32_t) { ++gpuInit; return true; }
GpuSlot& GpuGetSlot(uint32_t) { return slot; }
void GpuWaitSlot(GpuSlot&) {}
bool GpuEnsureRenderTargets(GpuSlot&, uint32_t, uint32_t, VkFormat) { ++renderTargets; return false; }
bool GpuEnsureHudless(GpuSlot&, uint32_t, uint32_t, VkFormat) { ++hudlessAllocs; return false; }
GpuImage* GpuEnsureSrOutput(uint32_t, uint32_t) { ++srAllocs; return dlssRuns ? &srOut : nullptr; }
void GpuUpdateDescriptors(GpuSlot&) {}
void GpuCollectGarbage(uint64_t) {}
VkPipelineLayout GpuPipelineLayout() { return {}; }
VkPipeline GpuCameraPipeline() { return {}; }
VkRenderPass GpuMvRenderPass() { return {}; }
VkPipeline GpuReplayPipeline(const PipeInfo&) { return {}; }
static VKAPI_ATTR VkResult VKAPI_CALL Submit(VkQueue, uint32_t, const VkSubmitInfo*, VkFence) { ++submits; return VK_SUCCESS; }
static VKAPI_ATTR VkResult VKAPI_CALL Present(VkQueue, const VkPresentInfoKHR*) { return VK_SUCCESS; }
static VKAPI_ATTR void VKAPI_CALL Bind(VkCommandBuffer, VkPipelineBindPoint, VkPipeline p) { bound = p; }
}

int main() {
  using namespace feverscaler;
  vk.vkQueueSubmit = Submit;
  vk.vkCmdBindPipeline = Bind;
  VkCommandBuffer cb = (VkCommandBuffer)2;
  CbState* st = GetCbState(cb);
  // Both features off: the real submit wrapper forwards once without opening a plugin frame,
  // initializing GPU resources, tagging or touching the HUD-less path.
  g_cfg.frameGeneration = g_cfg.superResolution = false;
  CHECK(w_QueueSubmit({}, 0, nullptr, {}) == VK_SUCCESS);
  OnUiPassBegin(cb, st);
  CHECK(submits == 1 && !g_frameOpen && gpuInit == 0 && renderTargets == 0 && tags == 0);
  CHECK(MsaaFor(8) == 8 && VsyncFor(true));
  // Availability alone never enables rendering; unavailable requested features do not either.
  g_cfg.superResolution = true;
  srReady = false;
  CHECK(!RenderWorkRequested() && MsaaFor(4) == 4);
  srReady = true;
  CHECK(RenderWorkRequested() && MsaaFor(4) == 1);
  g_srFailed = true;
  CHECK(!RenderWorkRequested() && MsaaFor(4) == 4);
  g_srFailed = false;

  // The pre-pass checks the real image metadata immediately before any GPU work.
  VkImage depth = (VkImage)3;
  g_mainDepth = depth;
  g_cam.valid = g_haveMain = true;
  g_mainW = 1920; g_mainH = 1080;
  for (auto samples : {VkSampleCountFlagBits(0), VK_SAMPLE_COUNT_2_BIT, VK_SAMPLE_COUNT_4_BIT, VK_SAMPLE_COUNT_8_BIT}) {
    g_imgs[depth] = {VK_FORMAT_D32_SFLOAT, 1920, 1080, VK_IMAGE_USAGE_TRANSFER_SRC_BIT, samples, {}};
    RunPrePass({}, nullptr);
    CHECK(renderTargets == 0 && tags == 0);
  }
  g_imgs[depth].samples = VK_SAMPLE_COUNT_1_BIT;
  g_imgs[depth].usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
  RunPrePass({}, nullptr);
  CHECK(renderTargets == 0);
  g_imgs[depth].usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  RunPrePass({}, nullptr);
  CHECK(renderTargets == 1); // positive control: a valid depth reaches allocation

  // Unknown scales reject every view. Known scales accept the world but reject previews,
  // including previews appearing first in a recording.
  SetSwapchainInfo({1920, 1080}, VK_FORMAT_R8G8B8A8_UNORM);
  slider = 0;
  CHECK(!IsWorldView(640, 360) && !IsWorldView(1920, 1080));
  slider = 0.5f;
  CHECK(IsWorldView(960, 540) && !IsWorldView(320, 180));
  VkRenderPass rp = (VkRenderPass)4;
  VkFramebuffer fb = (VkFramebuffer)5;
  VkImageView view = (VkImageView)6;
  RpInfo ri;
  ri.isMain = ri.hasDepth = true; ri.depthIndex = 0; ri.depthFormat = VK_FORMAT_D32_SFLOAT;
  g_rps[rp] = ri; g_views[view] = {depth}; g_fbs[fb] = {rp, {view}, 960, 540};
  VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  begin.renderPass = rp; begin.framebuffer = fb;
  g_imgs[depth].samples = VK_SAMPLE_COUNT_4_BIT;
  OnBeginRenderPass(cb, &begin);
  CHECK(st->hasMainPass && st->mainSamples == VK_SAMPLE_COUNT_4_BIT && st->mainDepth == depth);
  CHECK(WorldDepthImage() == depth); // available before ProcessCommandBuffer/submit
  g_fbs[fb].w = 320; g_fbs[fb].h = 180;
  OnBeginRenderPass(cb, &begin);
  CHECK(!st->inMainPass && !st->worldView && st->mainW == 960 && st->mainH == 540);
  CHECK(WorldDepthImage() == depth); // a preview cannot replace the recorded world depth

  // UI recording before the 3D submit still sees the recorded depth. A safe sample count
  // permits SR; the same ordering with MSAA must reject it before any output allocation.
  VkImage color = (VkImage)10;
  g_imgs[color] = {VK_FORMAT_R8G8B8A8_UNORM, 1920, 1080, VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_SAMPLE_COUNT_1_BIT, {}};
  st->hasMainPass = false; st->candImage = color; st->candW = 1920; st->candH = 1080;
  st->candLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  g_haveMain = false; g_srOn = true;
  OnUiPassBegin(cb, st);
  CHECK(srAllocs == 0 && hudlessAllocs == 0);
  g_imgs[depth].samples = VK_SAMPLE_COUNT_1_BIT;
  OnUiPassBegin(cb, st);
  CHECK(srAllocs == 1);

  // Pipeline changes require proven SR output and the world view, never previews.
  PipeInfo pipe; pipe.noA2C = (VkPipeline)8; pipe.srSsr = (VkPipeline)9;
  VkPipeline original = (VkPipeline)7;
  g_pipes[original] = &pipe;
  st->inMainPass = st->worldView = true;
  g_srOn = true; g_dlssLive = false;
  w_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, original); CHECK(bound == original);
  g_dlssLive = true;
  w_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, original); CHECK(bound == pipe.noA2C);
  st->inMainPass = false;
  w_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, original); CHECK(bound == pipe.srSsr);
  st->worldView = false;
  w_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, original); CHECK(bound == original);
  g_cfg.superResolution = false;
  CHECK(!SrActive());
  // Resetting accumulated frame data (e.g. many submits before a present) must not lose a
  // camera already jittered in this frame. Re-read its original matrices without jittering twice.
  alignas(16) uint8_t cameraBytes[464]{};
  float matrices[64]{};
  for (int m = 0; m < 4; ++m) for (int i = 0; i < 4; ++i) matrices[m * 16 + i * 5] = 1;
  matrices[32 + 11] = -1; matrices[32 + 15] = 0;
  memcpy(cameraBytes, matrices, sizeof(matrices));
  float nearZ = 1, farZ = 1000;
  memcpy(cameraBytes + 416, &nearZ, 4); memcpy(cameraBytes + 420, &farZ, 4);
  VkBuffer cameraBuffer = (VkBuffer)20;
  VkDeviceMemory cameraMemory = (VkDeviceMemory)21;
  VkDescriptorSet cameraSet = (VkDescriptorSet)22;
  g_bufs[cameraBuffer] = {sizeof(cameraBytes), cameraMemory, 0};
  g_mems[cameraMemory] = {cameraBytes, 0, sizeof(cameraBytes), sizeof(cameraBytes)};
  g_sets[cameraSet].b10 = {cameraBuffer, 0, sizeof(cameraBytes), false};
  g_cfg.superResolution = true; g_srOn = true; g_dlssLive = true;
  g_jitterW = 1280; g_mainW = 1280; g_mainH = 720; g_cam.valid = false;
  CameraBlockSeen(cameraSet);
  CHECK(g_cam.valid && !g_patches.empty());
  uint8_t onceJittered[256]; memcpy(onceJittered, cameraBytes, sizeof(onceJittered));
  g_cam.valid = false;
  CameraBlockSeen(cameraSet);
  CHECK(g_cam.valid && !memcmp(onceJittered, cameraBytes, sizeof(onceJittered)));
  // A runtime that never evaluates successfully gets a bounded startup attempt and then
  // restores the original renderer settings. FG, if requested separately, still needs MSAA off.
  g_cfg.superResolution = g_srWanted = true;
  g_dlssLive = false; g_dlssFrameOk = false; g_srScale = {}; g_srFailed = false;
  for (uint32_t i = 0; i < SrScalePolicy::kFailFrames; ++i) SrOnPresent(true);
  CHECK(!SrRequested() && !SrActive() && !SrGetStatus().available);
  CHECK(*SrGetStatus().unavailableReason && overrideScale == 0 && MsaaFor(8) == 8);
  g_cfg.frameGeneration = true;
  CHECK(RenderWorkRequested() && MsaaFor(8) == 1 && !VsyncFor(true));
  // Switching both features off must reject retained last-frame tags at present as well.
  g_cfg.frameGeneration = g_cfg.superResolution = false;
  g_tagged = g_haveMain = true;
  vk.vkQueuePresentKHR = Present;
  VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
  CHECK(w_QueuePresentKHR({}, &present) == VK_SUCCESS && !g_lastValid && !g_genOn);
  // Startup initializes context fields directly, without either settings setter. Capture that
  // baseline from the read-only query, apply temporary overrides once SL is ready, then restore.
  alignas(uintptr_t) uint8_t context[0x360]{};
  g_contextVtable = 1234;
  *reinterpret_cast<uintptr_t*>(context) = g_contextVtable;
  context[0x2A1] = 1;
  int four = 4; memcpy(context + 0x2DC, &four, sizeof(four));
  g_ctx = nullptr; g_gameVsync = g_gameMsaa = -1; g_pending = false;
  o_QueryContextState = [](void*) -> bool { return false; };
  o_SetVsync = [](void* p, bool on) { static_cast<uint8_t*>(p)[0x2A1] = on; };
  o_SetMsaa = [](void* p, int count) { memcpy(static_cast<uint8_t*>(p) + 0x2DC, &count, sizeof(count)); };
  CHECK(!h_QueryContextState(context) && g_ctx == context && g_gameVsync == 1 && g_gameMsaa == 4);
  g_cfg.frameGeneration = true; g_srFailed = false;
  GameSettingsPoll();
  CHECK(g_vsync == 0 && g_msaa == 1 && context[0x2A1] == 0);
  CHECK(!h_QueryContextState(context) && g_gameVsync == 1 && g_gameMsaa == 4);
  g_cfg.frameGeneration = false;
  GameSettingsPoll();
  int restored = 0; memcpy(&restored, context + 0x2DC, sizeof(restored));
  CHECK(context[0x2A1] == 1 && restored == 4);

  // Texture mip bias: while SR runs, world-pass binds get a copy of the game's set in which the
  // asset texture's sampler is a twin with the extra bias; render targets and compare samplers keep
  // the game's. Copies end only after the game rewrites, copies over or resets its sets.
  static std::unordered_map<VkSampler, float> samplerBias;
  static uint64_t nextHandle = 1000;
  static VkDescriptorSet boundSet;
  static std::vector<VkDescriptorImageInfo> written;  // image descriptors of the latest update
  static unsigned frees = 0;
  vk.vkCreateSampler = [](VkDevice, const VkSamplerCreateInfo* ci, const VkAllocationCallbacks*, VkSampler* out) {
    *out = (VkSampler)nextHandle++;
    samplerBias[*out] = ci->mipLodBias;
    return VK_SUCCESS;
  };
  vk.vkCreateDescriptorSetLayout = [](VkDevice, const VkDescriptorSetLayoutCreateInfo*, const VkAllocationCallbacks*,
                                      VkDescriptorSetLayout* out) { *out = (VkDescriptorSetLayout)nextHandle++; return VK_SUCCESS; };
  vk.vkCreateDescriptorPool = [](VkDevice, const VkDescriptorPoolCreateInfo*, const VkAllocationCallbacks*,
                                 VkDescriptorPool* out) { *out = (VkDescriptorPool)nextHandle++; return VK_SUCCESS; };
  vk.vkAllocateDescriptorSets = [](VkDevice, const VkDescriptorSetAllocateInfo* ai, VkDescriptorSet* out) {
    for (uint32_t i = 0; i < ai->descriptorSetCount; ++i) out[i] = (VkDescriptorSet)nextHandle++;
    return VK_SUCCESS;
  };
  vk.vkFreeDescriptorSets = [](VkDevice, VkDescriptorPool, uint32_t n, const VkDescriptorSet*) { frees += n; return VK_SUCCESS; };
  vk.vkResetDescriptorPool = [](VkDevice, VkDescriptorPool, VkDescriptorPoolResetFlags) { return VK_SUCCESS; };
  vk.vkUpdateDescriptorSets = [](VkDevice, uint32_t n, const VkWriteDescriptorSet* w, uint32_t, const VkCopyDescriptorSet*) {
    written.clear();
    for (uint32_t i = 0; i < n; ++i)
      if (w[i].pImageInfo) written.push_back(w[i].pImageInfo[0]);
  };
  vk.vkCmdBindDescriptorSets = [](VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout, uint32_t, uint32_t,
                                  const VkDescriptorSet* s, uint32_t, const uint32_t*) { boundSet = s[0]; };
  vk.vkGetPhysicalDeviceProperties = [](VkPhysicalDevice, VkPhysicalDeviceProperties* p) { p->limits.maxSamplerLodBias = 15; };
  auto createSampler = (PFN_vkCreateSampler)MipBiasWrapper("vkCreateSampler");
  auto createLayout = (PFN_vkCreateDescriptorSetLayout)MipBiasWrapper("vkCreateDescriptorSetLayout");
  auto allocate = (PFN_vkAllocateDescriptorSets)MipBiasWrapper("vkAllocateDescriptorSets");
  auto resetPool = (PFN_vkResetDescriptorPool)MipBiasWrapper("vkResetDescriptorPool");
  VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sci.mipLodBias = -0.5f;
  sci.maxLod = 1000;
  VkSampler trilinear, compare;
  CHECK(createSampler(gDevice, &sci, nullptr, &trilinear) == VK_SUCCESS);
  sci.compareEnable = VK_TRUE;
  CHECK(createSampler(gDevice, &sci, nullptr, &compare) == VK_SUCCESS);
  const VkDescriptorType combined = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  VkDescriptorSetLayoutBinding lb[4] = {{25, combined, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                                        {3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                                        {23, combined, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                                        {24, combined, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
  VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  lci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;  // as TF3 creates all its layouts
  lci.bindingCount = 4;
  lci.pBindings = lb;
  VkDescriptorSetLayout layout;
  CHECK(createLayout(gDevice, &lci, nullptr, &layout) == VK_SUCCESS);
  VkDescriptorPool gamePool = (VkDescriptorPool)900;
  VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  ai.descriptorPool = gamePool;
  ai.descriptorSetCount = 1;
  ai.pSetLayouts = &layout;
  VkDescriptorSet set, set2;
  CHECK(allocate(gDevice, &ai, &set) == VK_SUCCESS);
  VkImage tex = (VkImage)901, target = (VkImage)902;
  VkImageView texView = (VkImageView)903, targetView = (VkImageView)904;
  g_imgs[tex] = {VK_FORMAT_BC1_RGB_UNORM_BLOCK, 512, 512, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                 VK_SAMPLE_COUNT_1_BIT, {texView}, 10};
  g_imgs[target] = {VK_FORMAT_R16G16B16A16_SFLOAT, 256, 256, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                    VK_SAMPLE_COUNT_1_BIT, {targetView}, 9};
  g_views[texView] = {tex};
  g_views[targetView] = {target};
  const VkImageLayout ro = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  VkDescriptorImageInfo ii[3] = {{trilinear, texView, ro}, {trilinear, targetView, ro}, {compare, texView, ro}};
  VkDescriptorBufferInfo ubo{(VkBuffer)905, 0, 64};
  VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
  w[0].dstSet = set; w[0].dstBinding = 23; w[0].descriptorCount = 3; w[0].descriptorType = combined; w[0].pImageInfo = ii;
  w[1].dstSet = set; w[1].dstBinding = 3; w[1].descriptorCount = 1;
  w[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[1].pBufferInfo = &ubo;
  w_UpdateDescriptorSets(gDevice, 2, w, 0, nullptr);  // one write runs on through bindings 24 and 25
  auto bind = [&](VkDescriptorSet s) { w_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, {}, 0, 1, &s, 0, nullptr); };
  g_cfg.superResolution = true; g_srFailed = false; g_srOn = true; g_dlssLive = true;
  st->inMainPass = true;
  MipBiasOnPresent(1.0f, 1);  // DLAA: nothing to correct
  bind(set);
  CHECK(boundSet == set);
  const float ratio = 1147.0f / 3440.0f;
  MipBiasOnPresent(ratio, 2);
  bind(set);
  VkDescriptorSet copy = boundSet;
  CHECK(copy != set && written.size() == 3);
  CHECK(written[0].sampler != trilinear && written[0].imageView == texView);
  CHECK(std::fabs(samplerBias[written[0].sampler] - (-0.5f + std::lround(std::log2(ratio) * 32) / 32.0f)) < 1e-5f);
  CHECK(written[1].sampler == trilinear && written[2].sampler == compare);
  VkSampler twin = written[0].sampler;
  bind(set);
  CHECK(boundSet == copy);  // made once
  st->inMainPass = false;
  bind(set);
  CHECK(boundSet == set);  // not the world pass
  st->inMainPass = true;
  g_dlssLive = false;
  bind(set);
  CHECK(boundSet == set);  // DLSS not producing the image
  g_dlssLive = true;
  // A rewrite retires the copy: freed 8 presents later; the next bind makes a new one.
  w_UpdateDescriptorSets(gDevice, 1, &w[1], 0, nullptr);
  MipBiasOnPresent(ratio, 9);
  CHECK(frees == 0);
  MipBiasOnPresent(ratio, 10);
  CHECK(frees == 1);
  bind(set);
  CHECK(boundSet != set && boundSet != copy && written[0].sampler == twin);
  // The game's copy-on-write (vkCopyDescriptorSet into a new set) keeps the bias.
  CHECK(allocate(gDevice, &ai, &set2) == VK_SUCCESS);
  VkCopyDescriptorSet cp[2] = {{VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET}};
  cp[0].srcSet = set; cp[0].srcBinding = 23; cp[0].dstSet = set2; cp[0].dstBinding = 23; cp[0].descriptorCount = 3;
  cp[1].srcSet = set; cp[1].srcBinding = 3; cp[1].dstSet = set2; cp[1].dstBinding = 3; cp[1].descriptorCount = 1;
  w_UpdateDescriptorSets(gDevice, 0, nullptr, 2, cp);
  bind(set2);
  CHECK(boundSet != set2 && written.size() == 3 && written[0].sampler == twin && written[2].sampler == compare);
  // A pool reset forgets its sets; their copies go 8 presents later.
  CHECK(resetPool(gDevice, gamePool, 0) == VK_SUCCESS);
  MipBiasOnPresent(ratio, 18);
  CHECK(frees == 3);
  bind(set);
  CHECK(boundSet == set);

  // DLSS output hand-off. Without FSR1 (slider above 95 %) the image the UI pass stretches onto the
  // screen is the post-compose scene copy, which the tonemapper also reads. Only the UI pass may
  // sample the DLSS output: the tonemapper fed it re-tonemaps it every frame into a grey screen.
  vk.vkCmdPipelineBarrier = [](VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags, uint32_t,
                               const VkMemoryBarrier*, uint32_t, const VkBufferMemoryBarrier*, uint32_t,
                               const VkImageMemoryBarrier*) {};
  VkDescriptorSetLayoutBinding texBinding{0, combined, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
  lci.bindingCount = 1;
  lci.pBindings = &texBinding;
  VkDescriptorSetLayout texLayout;
  CHECK(createLayout(gDevice, &lci, nullptr, &texLayout) == VK_SUCCESS);
  ai.pSetLayouts = &texLayout;
  VkDescriptorSet composeSet, uiSet, iconSet;
  CHECK(allocate(gDevice, &ai, &composeSet) == VK_SUCCESS && allocate(gDevice, &ai, &uiSet) == VK_SUCCESS &&
        allocate(gDevice, &ai, &iconSet) == VK_SUCCESS);
  VkImage scene = (VkImage)950;
  VkImageView sceneView = (VkImageView)951;
  g_imgs[scene] = {VK_FORMAT_R16G16B16A16_SFLOAT, 1280, 720,
                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                   VK_SAMPLE_COUNT_1_BIT, {sceneView}};
  g_views[sceneView] = {scene};
  VkDescriptorImageInfo sceneInfo{trilinear, sceneView, ro}, iconInfo{trilinear, texView, ro};
  VkWriteDescriptorSet tw{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  tw.dstBinding = 0; tw.descriptorCount = 1; tw.descriptorType = combined;
  auto writeTexture = [&](VkDescriptorSet s, const VkDescriptorImageInfo* info) {
    tw.dstSet = s; tw.pImageInfo = info;
    w_UpdateDescriptorSets(gDevice, 1, &tw, 0, nullptr);
  };
  writeTexture(composeSet, &sceneInfo);
  writeTexture(uiSet, &sceneInfo);
  writeTexture(iconSet, &iconInfo);
  srOut = {(VkImage)952, (VkDeviceMemory)953, (VkImageView)954, VK_FORMAT_R16G16B16A16_SFLOAT, 1920, 1080,
           VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
  slot.depth.image = (VkImage)955; slot.depth.w = 1280; slot.depth.h = 720;
  slot.mv.image = (VkImage)956; slot.mv.w = 1280; slot.mv.h = 720;
  g_cfg.superResolution = true; g_cfg.frameGeneration = false; g_srFailed = false; g_srOn = true;
  g_dlssFrameOk = false;
  VkCommandBuffer uiCb = (VkCommandBuffer)957;
  CbState* ui = GetCbState(uiCb);
  auto bindIn = [&](VkCommandBuffer c, VkDescriptorSet s) {
    w_CmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, {}, 0, 1, &s, 0, nullptr);
  };
  auto recordFrame = [&](bool dlss) {
    dlssRuns = dlss;
    g_cam.valid = true;  // a submit starting a frame clears it
    ui->Reset();
    ui->candImage = ui->srIn = scene;
    ui->candW = ui->srInW = 1280; ui->candH = ui->srInH = 720;
    ui->candLayout = ui->srInLayout = ro;
    bindIn(uiCb, composeSet);  // the tonemapper, before the UI pass
    CHECK(boundSet == composeSet);
    OnUiPassBegin(uiCb, ui);
  };
  auto submitUi = [&] {
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1; si.pCommandBuffers = &uiCb;
    CHECK(w_QueueSubmit({}, 1, &si, {}) == VK_SUCCESS);
  };
  recordFrame(true);
  CHECK(ui->sceneOut == srOut.view);
  bindIn(uiCb, iconSet);
  CHECK(boundSet == iconSet && !ui->sceneShown);  // other UI textures stay the game's
  submitUi();
  CHECK(!g_dlssFrameOk);  // evaluated but never on screen: not a DLSS frame
  recordFrame(true);
  bindIn(uiCb, uiSet);
  VkDescriptorSet uiCopy = boundSet;
  CHECK(uiCopy != uiSet && written.size() == 1 && written[0].imageView == srOut.view && written[0].sampler == trilinear);
  bindIn(uiCb, uiSet);
  CHECK(boundSet == uiCopy && ui->sceneShown);
  submitUi();
  CHECK(g_dlssFrameOk);
  // The game's own sets never sample the DLSS output: a rewrite of the tonemapper's set reaches the
  // driver unchanged, and the next frame's tonemapper binds it as is.
  writeTexture(composeSet, &sceneInfo);
  CHECK(written.size() == 1 && written[0].imageView == sceneView);
  recordFrame(true);
  // No DLSS image in a frame: the UI pass shows the game's picture.
  recordFrame(false);
  CHECK(!ui->sceneOut);
  bindIn(uiCb, uiSet);
  CHECK(boundSet == uiSet);
  std::puts("PASS: off switches, unsupported features, MSAA guards, world/preview identity, SR-only pipelines, mip bias "
            "copies and the UI-only DLSS hand-off");
}
