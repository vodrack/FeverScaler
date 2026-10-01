// Streamline bring-up and per-frame calls. Preferences, the NGX application id and the
// "one frame token per frame" marker discipline follow bg3fgvk (MIT), which runs this exact
// Streamline 2.12.0 + DLSS-G 310.7 runtime on Vulkan (including RTX 30 via RTX30MFG-Unlock).
#include "sl_bridge.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <string>

#include <sl_consts.h>
#include <sl_core_api.h>
#include <sl_dlss.h>
#include <sl_dlss_g.h>
#include <sl_helpers_vk.h>
#include <sl_pcl.h>
#include <sl_reflex.h>

#include "config.h"
#include "diagnostics.h"
#include "log.h"

namespace feverscaler {
static HMODULE g_sl{};
static std::wstring g_slDir;
static PFun_slInit* p_slInit{};
static PFun_slIsFeatureSupported* p_slIsFeatureSupported{};
static PFun_slGetFeatureFunction* p_slGetFeatureFunction{};
static PFun_slGetNewFrameToken* p_slGetNewFrameToken{};
static PFun_slSetTagForFrame* p_slSetTagForFrame{};
static PFun_slSetConstants* p_slSetConstants{};
static PFun_slReflexSetOptions* p_slReflexSetOptions{};
static PFun_slReflexSleep* p_slReflexSleep{};
static PFun_slPCLSetMarker* p_slPCLSetMarker{};
static PFun_slDLSSGSetOptions* p_slDLSSGSetOptions{};
static PFun_slDLSSGGetState* p_slDLSSGGetState{};
static PFun_slEvaluateFeature* p_slEvaluateFeature{};
static PFun_slDLSSSetOptions* p_slDLSSSetOptions{};
static PFun_slDLSSGetOptimalSettings* p_slDLSSGetOptimalSettings{};
static PFun_slDLSSGetState* p_slDLSSGetState{};
static std::atomic<uint64_t> g_vramWarnTick{0};

// Streamline results: warnings (e.g. eWarnOutOfVRAM) mean the call did its work.
static bool SlSucceeded(sl::Result r, const char* what) {
  if (r == sl::Result::eOk) return true;
  if (r == sl::Result::eWarnOutOfVRAM) {
    g_vramWarnTick = GetTickCount64();
    LogEvent("out-of-vram", "%s: Streamline reports the GPU is out of VRAM budget", what);
    return true;
  }
  LogEvent(what, "%s: Streamline result=%d", what, (int)r);
  return false;
}
static bool g_core = false;   // Streamline runs on the game's device (frame tokens, tags, constants)
static bool g_ready = false;  // DLSS-G, Reflex and PCL are usable as well

// Why a feature cannot run, for the user: written while Streamline starts, before the first present.
static char g_srWhy[192] = "Streamline did not start (see feverscaler.log)";
static char g_fgWhy[192] = "Streamline did not start (see feverscaler.log)";
static void SetWhy(char* dst, const char* text) { snprintf(dst, sizeof(g_srWhy), "%s", text); }
static void SetWhyBoth(const char* text) {
  SetWhy(g_srWhy, text);
  SetWhy(g_fgWhy, text);
}
// The Streamline results a user can act on.
static const char* ResultText(sl::Result r) {
  switch (r) {
    case sl::Result::eErrorDriverOutOfDate: return "the NVIDIA driver is too old - update it";
    case sl::Result::eErrorOSOutOfDate: return "Windows is too old for it - update Windows";
    case sl::Result::eErrorOSDisabledHWS:
      return "Hardware-accelerated GPU scheduling is off (Windows Settings > System > Display > Graphics)";
    case sl::Result::eErrorNoSupportedAdapterFound:
    case sl::Result::eErrorAdapterNotSupported:
    case sl::Result::eErrorFeatureNotSupported: return "not supported by this graphics card (NVIDIA RTX required)";
    case sl::Result::eErrorNGXFailed: return "NVIDIA NGX failed to start (graphics driver problem?)";
    case sl::Result::eErrorNoPlugins:
    case sl::Result::eErrorFeatureMissing:
    case sl::Result::eErrorFeatureFailedToLoad: return "Streamline files are missing or failed to load - reinstall FeverScaler";
    default: return nullptr;
  }
}
static void SetWhyResult(char* dst, const char* prefix, sl::Result r) {
  char text[192];
  if (const char* known = ResultText(r)) snprintf(text, sizeof(text), "%s%s", prefix, known);
  else snprintf(text, sizeof(text), "%sStreamline result %d (see feverscaler.log)", prefix, (int)r);
  SetWhy(dst, text);
}
// A feature whose functions are missing after device creation: ask Streamline why.
static void FeatureWhy(char* dst, sl::Feature f, VkPhysicalDevice pd) {
  sl::Result r = sl::Result::eOk;
  if (p_slIsFeatureSupported) {
    sl::AdapterInfo ai{};
    ai.vkPhysicalDevice = pd;
    r = p_slIsFeatureSupported(f, ai);
  }
  if (r == sl::Result::eOk) SetWhy(dst, "could not be started on this graphics card (see feverscaler.log)");
  else SetWhyResult(dst, "", r);
}

static void StreamlineLog(sl::LogType type, const char* message) {
  if (!message) return;
  if (type == sl::LogType::eInfo) {
    if (Cfg().slLogLevel >= 2) Log("Streamline: %s", message);
    return;
  }
  const char* key = type == sl::LogType::eError ? "SL-error" : "SL-warning";
  char sourceKey[96];
  // Stable call-site keys ignore changing timestamps, thread ids, and frame numbers.
  if (const char* source = strstr(message, ".cpp:")) {
    const char* begin = source;
    while (begin > message && begin[-1] != ' ' && begin[-1] != ']') --begin;
    const char* end = strchr(source, ']');
    if (end) {
      snprintf(sourceKey, sizeof(sourceKey), "SL-%u-%.*s", (unsigned)type,
          (int)((end - begin + 1) < 80 ? (end - begin + 1) : 80), begin);
      key = sourceKey;
    }
  }
  LogEvent(key, "%s", message);
}

static HMODULE LoadInterposer() {
  if (g_sl) return g_sl;
  g_sl = GetModuleHandleW(L"sl.interposer.dll");
  if (g_sl) {
    Log("sl.interposer.dll already loaded (%p) - reusing", (void*)g_sl);
    return g_sl;
  }
  g_slDir = PluginDir() + L"feverscaler\\Streamline";
  std::wstring path = g_slDir + L"\\sl.interposer.dll";
  g_sl = LoadLibraryW(path.c_str());
  Log("Streamline runtime %S: %s", path.c_str(), g_sl ? "loaded" : "NOT FOUND");
  if (!g_sl) SetWhyBoth("Streamline runtime not found in scripts/feverscaler/Streamline - reinstall FeverScaler");
  return g_sl;
}

void* SlProxyFn(const char* name) {
  HMODULE m = LoadInterposer();
  return m ? (void*)GetProcAddress(m, name) : nullptr;
}

bool SlInit() {
  if (!LoadInterposer()) return false;
  p_slInit = (PFun_slInit*)GetProcAddress(g_sl, "slInit");
  p_slGetFeatureFunction = (PFun_slGetFeatureFunction*)GetProcAddress(g_sl, "slGetFeatureFunction");
  p_slGetNewFrameToken = (PFun_slGetNewFrameToken*)GetProcAddress(g_sl, "slGetNewFrameToken");
  p_slSetTagForFrame = (PFun_slSetTagForFrame*)GetProcAddress(g_sl, "slSetTagForFrame");
  p_slSetConstants = (PFun_slSetConstants*)GetProcAddress(g_sl, "slSetConstants");
  p_slEvaluateFeature = (PFun_slEvaluateFeature*)GetProcAddress(g_sl, "slEvaluateFeature");
  p_slIsFeatureSupported = (PFun_slIsFeatureSupported*)GetProcAddress(g_sl, "slIsFeatureSupported");
  if (!p_slInit || !p_slGetFeatureFunction || !p_slGetNewFrameToken || !p_slSetTagForFrame || !p_slSetConstants) {
    Log("SlInit: core exports missing");
    SetWhyBoth("the Streamline runtime is incomplete - reinstall FeverScaler");
    return false;
  }
  sl::Preferences p{};
  static const sl::Feature feats[] = {sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL, sl::kFeatureDLSS};
  p.featuresToLoad = feats;
  p.numFeaturesToLoad = 4;
  // Manual hooking: we drive the interposer from our vkGetInstanceProcAddr detour; without it
  // slInit installs its own Vulkan interception and collides with ours.
  p.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseManualHooking |
            sl::PreferenceFlags::eUseFrameBasedResourceTagging;
  p.renderAPI = sl::RenderAPI::eVulkan;
  // NGX application id family the driver serves DLSS-G minimum-spec data for (bg3fgvk finding:
  // the SDK sample id has none and DLSS-G never arms).
  p.applicationId = 0xE658703;
  p.logLevel = Cfg().slLogLevel == 0 ? sl::LogLevel::eOff
             : Cfg().slLogLevel == 1 ? sl::LogLevel::eDefault
                                     : sl::LogLevel::eVerbose;
  // The SDK explicitly supports nullptr to disable its unbounded file sink.
  // Warning/error callbacks remain enabled even when info logging is off.
  p.pathToLogsAndData = nullptr;
  p.logMessageCallback = StreamlineLog;
  static const wchar_t* paths[1];
  paths[0] = g_slDir.c_str();
  p.pathsToPlugins = paths;
  p.numPathsToPlugins = 1;
  sl::Result r = p_slInit(p, sl::kSDKVersion);
  Log("slInit -> %d (plugins from %S)", (int)r, g_slDir.c_str());
  if (r != sl::Result::eOk) {
    SetWhyResult(g_srWhy, "Streamline could not start: ", r);
    SetWhyResult(g_fgWhy, "Streamline could not start: ", r);
  }
  return r == sl::Result::eOk;
}

void SlOnDeviceCreated(VkPhysicalDevice physicalDevice) {
  auto feat = [](sl::Feature f, const char* name) {
    void* fn = nullptr;
    sl::Result r = p_slGetFeatureFunction(f, name, fn);
    if (r != sl::Result::eOk || !fn) Log("slGetFeatureFunction(%s) -> %d", name, (int)r);
    return fn;
  };
  p_slReflexSetOptions = (PFun_slReflexSetOptions*)feat(sl::kFeatureReflex, "slReflexSetOptions");
  p_slReflexSleep = (PFun_slReflexSleep*)feat(sl::kFeatureReflex, "slReflexSleep");
  p_slPCLSetMarker = (PFun_slPCLSetMarker*)feat(sl::kFeaturePCL, "slPCLSetMarker");
  p_slDLSSGSetOptions = (PFun_slDLSSGSetOptions*)feat(sl::kFeatureDLSS_G, "slDLSSGSetOptions");
  p_slDLSSGGetState = (PFun_slDLSSGGetState*)feat(sl::kFeatureDLSS_G, "slDLSSGGetState");
  p_slDLSSSetOptions = (PFun_slDLSSSetOptions*)feat(sl::kFeatureDLSS, "slDLSSSetOptions");
  p_slDLSSGetOptimalSettings = (PFun_slDLSSGetOptimalSettings*)feat(sl::kFeatureDLSS, "slDLSSGetOptimalSettings");
  p_slDLSSGetState = (PFun_slDLSSGetState*)feat(sl::kFeatureDLSS, "slDLSSGetState");
  // Super Resolution and frame generation are independent: either runs without the other.
  g_core = true;
  if (SlDlssAvailable()) SetWhy(g_srWhy, "");
  else FeatureWhy(g_srWhy, sl::kFeatureDLSS, physicalDevice);
  Log("DLSS Super Resolution: %s%s", SlDlssAvailable() ? "available" : "NOT available: ", g_srWhy);
  g_ready = p_slReflexSetOptions && p_slPCLSetMarker && p_slDLSSGSetOptions && p_slDLSSGGetState;
  if (!g_ready) {
    if (!p_slDLSSGSetOptions || !p_slDLSSGGetState) FeatureWhy(g_fgWhy, sl::kFeatureDLSS_G, physicalDevice);
    else if (!p_slReflexSetOptions) FeatureWhy(g_fgWhy, sl::kFeatureReflex, physicalDevice);
    else FeatureWhy(g_fgWhy, sl::kFeaturePCL, physicalDevice);
    Log("DLSS Frame Generation NOT available: %s", g_fgWhy);
    return;
  }
  SetWhy(g_fgWhy, "");
  sl::ReflexOptions ro{};
  ro.mode = Cfg().reflexMode == 0   ? sl::ReflexMode::eOff
            : Cfg().reflexMode == 1 ? sl::ReflexMode::eLowLatency
                                    : sl::ReflexMode::eLowLatencyWithBoost;
  sl::Result rr = p_slReflexSetOptions(ro);
  Log("Streamline ready: Reflex mode %d -> %d", Cfg().reflexMode, (int)rr);
}

bool SlCoreReady() { return g_core; }
bool SlFgReady() { return g_ready; }
const char* SlSrUnavailableReason() { return g_srWhy; }
const char* SlFgUnavailableReason() { return g_fgWhy; }

// ---- frame tokens --------------------------------------------------------------------------------
// One slGetNewFrameToken call per frame (it advances SL's global counter on every call); the
// present markers carry the frame index DLSS-G uses to find that frame's tags and constants.
static std::mutex g_tokMutex;
static sl::FrameToken* g_token = nullptr;
static bool g_simStarted = false, g_midSent = false;

static void Marker(sl::PCLMarker m) {
  if (p_slPCLSetMarker && g_token) p_slPCLSetMarker(m, *g_token);
}
static void EnsureToken() {
  if (g_token || !p_slGetNewFrameToken) return;
  sl::FrameToken* t = nullptr;
  if (p_slGetNewFrameToken(t, nullptr) == sl::Result::eOk) g_token = t;
  g_simStarted = g_midSent = false;
}
static void SimStart() {
  if (g_simStarted) return;
  g_simStarted = true;
  Marker(sl::PCLMarker::eSimulationStart);
}

const sl::FrameToken* SlCurrentToken() {
  std::lock_guard<std::mutex> lk(g_tokMutex);
  if (!g_core) return nullptr;
  EnsureToken();
  return g_token;
}

void SlOnFirstSubmit() {
  std::lock_guard<std::mutex> lk(g_tokMutex);
  if (!g_core) return;
  EnsureToken();
  SimStart();
  if (!g_midSent) {
    g_midSent = true;
    Marker(sl::PCLMarker::eSimulationEnd);
    Marker(sl::PCLMarker::eRenderSubmitStart);
  }
}

void SlOnPresentBegin() {
  std::lock_guard<std::mutex> lk(g_tokMutex);
  if (!g_core) return;
  EnsureToken();
  SimStart();
  if (!g_midSent) {
    g_midSent = true;
    Marker(sl::PCLMarker::eSimulationEnd);
    Marker(sl::PCLMarker::eRenderSubmitStart);
  }
  Marker(sl::PCLMarker::eRenderSubmitEnd);
  Marker(sl::PCLMarker::ePresentStart);
}

void SlOnPresentEnd() {
  sl::FrameToken* next = nullptr;
  {
    std::lock_guard<std::mutex> lk(g_tokMutex);
    if (!g_core) return;
    Marker(sl::PCLMarker::ePresentEnd);
    g_token = nullptr;
    EnsureToken();
    next = g_token;
  }
  if (next && g_ready && Cfg().reflexSleep && p_slReflexSleep) {  // Reflex is configured with DLSS-G only
    DiagnosticScope scope(DiagnosticStage::ReflexSleep);
    SlSucceeded(p_slReflexSleep(*next), "Reflex sleep");
  }
  std::lock_guard<std::mutex> lk(g_tokMutex);
  SimStart();
}

// ---- tags / constants ----------------------------------------------------------------------------
static sl::Resource MakeRes(const SlImage& im) {
  sl::Resource r(sl::ResourceType::eTex2d, (void*)im.image, (void*)im.memory, (void*)im.view, (uint32_t)im.layout);
  r.width = im.width;
  r.height = im.height;
  r.nativeFormat = (uint32_t)im.format;
  r.mipLevels = 1;
  r.arrayLayers = 1;
  r.usage = (uint32_t)im.usage;
  return r;
}

bool SlTagFrame(const SlImage& depth, const SlImage& mvec, const SlImage* hudless) {
  const sl::FrameToken* tok = SlCurrentToken();
  if (!tok) return false;
  sl::Resource d = MakeRes(depth), m = MakeRes(mvec), h{};
  sl::Extent de{0, 0, depth.width, depth.height}, me{0, 0, mvec.width, mvec.height}, he{};
  sl::ResourceTag tags[3] = {
      sl::ResourceTag(&d, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &de),
      sl::ResourceTag(&m, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &me),
  };
  uint32_t n = 2;
  if (hudless && hudless->image) {
    h = MakeRes(*hudless);
    he = sl::Extent{0, 0, hudless->width, hudless->height};
    tags[n++] = sl::ResourceTag(&h, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent, &he);
  }
  sl::ViewportHandle vp{0};
  sl::Result r = p_slSetTagForFrame(*tok, vp, tags, n, nullptr);
  if (SlSucceeded(r, "slSetTagForFrame")) r = sl::Result::eOk;
  if (r != sl::Result::eOk) FEVERSCALER_LOG_N(10, "slSetTagForFrame -> %d", (int)r);
  else FEVERSCALER_LOG_N(1, "slSetTagForFrame ok: depth %ux%u mvec %ux%u hudless %ux%u", depth.width, depth.height,
                   mvec.width, mvec.height, hudless ? hudless->width : 0, hudless ? hudless->height : 0);
  return r == sl::Result::eOk;
}

void SlClearTags() {
  const sl::FrameToken* tok = SlCurrentToken();
  if (!tok) return;
  sl::ResourceTag tags[3] = {
      sl::ResourceTag(nullptr, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent),
      sl::ResourceTag(nullptr, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent),
      sl::ResourceTag(nullptr, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent),
  };
  sl::ViewportHandle vp{0};
  p_slSetTagForFrame(*tok, vp, tags, 3, nullptr);
}

bool SlSetConstants(const sl::Constants& c) {
  const sl::FrameToken* tok = SlCurrentToken();
  if (!tok) return false;
  sl::ViewportHandle vp{0};
  sl::Result r = p_slSetConstants(c, *tok, vp);
  bool ok = SlSucceeded(r, "slSetConstants");
  if (!ok) FEVERSCALER_LOG_N(10, "slSetConstants -> %d", (int)r);
  return ok;
}

// ---- DLSS Super Resolution --------------------------------------------------------------------------
bool SlDlssAvailable() { return g_core && p_slDLSSSetOptions && p_slDLSSGetOptimalSettings && p_slEvaluateFeature; }

static sl::DLSSMode g_dlssMode = sl::DLSSMode::eOff;
static const char* ModeName(sl::DLSSMode m) {
  switch (m) {
    case sl::DLSSMode::eDLAA: return "DLAA";
    case sl::DLSSMode::eMaxQuality: return "Quality";
    case sl::DLSSMode::eBalanced: return "Balanced";
    case sl::DLSSMode::eMaxPerformance: return "Performance";
    case sl::DLSSMode::eUltraPerformance: return "Ultra Performance";
    case sl::DLSSMode::eUltraQuality: return "Ultra Quality";
    default: return "off";
  }
}
const char* SlDlssModeName() { return ModeName(g_dlssMode); }

bool SlDlssConfigure(uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, uint32_t preset) {
  if (!SlDlssAvailable()) return false;
  static uint32_t last[5] = {};
  static bool lastOk = false;
  uint32_t key[5] = {rw, rh, ow, oh, preset};
  if (!memcmp(key, last, sizeof(key))) return lastOk;
  memcpy(last, key, sizeof(key));
  lastOk = false;
  sl::DLSSOptions o{};
  o.outputWidth = ow;
  o.outputHeight = oh;
  o.colorBuffersHDR = sl::Boolean::eFalse;  // the game's tonemapped, gamma-encoded image
  sl::DLSSPreset p = (sl::DLSSPreset)preset;
  o.dlaaPreset = o.qualityPreset = o.balancedPreset = o.performancePreset = o.ultraPerformancePreset =
      o.ultraQualityPreset = p;
  sl::DLSSMode mode = sl::DLSSMode::eOff;
  if (rw >= ow && rh >= oh) {
    mode = sl::DLSSMode::eDLAA;
  } else {
    const sl::DLSSMode order[] = {sl::DLSSMode::eUltraQuality, sl::DLSSMode::eMaxQuality, sl::DLSSMode::eBalanced,
                                  sl::DLSSMode::eMaxPerformance, sl::DLSSMode::eUltraPerformance};
    // Every mode accepts a range of render sizes; use the one designed for this scale (closest
    // optimal size), e.g. Performance at 50 %, Quality at 67 %.
    uint32_t bestDiff = UINT32_MAX;
    for (sl::DLSSMode m : order) {
      o.mode = m;
      sl::DLSSOptimalSettings st{};
      if (p_slDLSSGetOptimalSettings(o, st) != sl::Result::eOk || !st.optimalRenderWidth) continue;
      Log("DLSS %s: optimal %ux%u, range %ux%u .. %ux%u", ModeName(m), st.optimalRenderWidth, st.optimalRenderHeight,
          st.renderWidthMin, st.renderHeightMin, st.renderWidthMax, st.renderHeightMax);
      if (rw < st.renderWidthMin || rw > st.renderWidthMax || rh < st.renderHeightMin || rh > st.renderHeightMax) continue;
      uint32_t diff = rw > st.optimalRenderWidth ? rw - st.optimalRenderWidth : st.optimalRenderWidth - rw;
      if (diff < bestDiff) {
        bestDiff = diff;
        mode = m;
      }
    }
  }
  if (mode == sl::DLSSMode::eOff) {
    Log("DLSS: no mode accepts render %ux%u for output %ux%u", rw, rh, ow, oh);
    g_dlssMode = mode;
    return false;
  }
  o.mode = mode;
  sl::ViewportHandle vp{0};
  sl::Result r = p_slDLSSSetOptions(vp, o);
  g_dlssMode = mode;
  lastOk = r == sl::Result::eOk;
  Log("DLSS %s: render %ux%u -> %ux%u, preset %u -> %d", ModeName(mode), rw, rh, ow, oh, preset, (int)r);
  return lastOk;
}

bool SlDlssEvaluate(VkCommandBuffer cmd, const SlImage& in, const SlImage& out, const SlImage& depth,
                    const SlImage& mvec) {
  DiagnosticScope scope(DiagnosticStage::DlssEvaluate);
  const sl::FrameToken* tok = SlCurrentToken();
  if (!tok || !SlDlssAvailable()) return false;
  sl::Resource ri = MakeRes(in), ro = MakeRes(out), rd = MakeRes(depth), rm = MakeRes(mvec);
  sl::Extent ei{0, 0, in.width, in.height}, eo{0, 0, out.width, out.height}, ed{0, 0, depth.width, depth.height},
      em{0, 0, mvec.width, mvec.height};
  sl::ResourceTag ti(&ri, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &ei);
  sl::ResourceTag to(&ro, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &eo);
  sl::ResourceTag td(&rd, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &ed);
  sl::ResourceTag tm(&rm, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &em);
  sl::ViewportHandle vp{0};
  const sl::BaseStructure* inputs[] = {&vp, &ti, &to, &td, &tm};
  sl::Result r = p_slEvaluateFeature(sl::kFeatureDLSS, *tok, inputs, 5, (sl::CommandBuffer*)cmd);
  bool ok = SlSucceeded(r, "DLSS evaluate");
  if (!ok) FEVERSCALER_LOG_N(20, "slEvaluateFeature(DLSS) -> %d", (int)r);
  else FEVERSCALER_LOG_N(1, "DLSS evaluate ok: %ux%u -> %ux%u", in.width, in.height, out.width, out.height);
  return ok;
}

// ---- DLSS-G control -------------------------------------------------------------------------------
static uint32_t g_framesMax = 0;
static int g_optionsError = 0;

bool SlSetGeneration(bool on, uint32_t frames, bool release) {
  DiagnosticScope scope(DiagnosticStage::FgOptions);
  if (!p_slDLSSGSetOptions) return false;
  sl::DLSSGOptions o{};
  o.mode = on ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
  if (frames < 1) frames = 1;
  if (frames > kMaxGeneratedFrames) frames = kMaxGeneratedFrames;
  uint32_t maxFrames = g_framesMax ? g_framesMax : 1;  // unknown capability: single FG only
  if (frames > maxFrames) frames = maxFrames;
  o.numFramesToGenerate = frames;
  // Retain resources when off (menus, loading): a free + re-create costs a >100 ms frame each time.
  // Switched off by the user: free them, they can be a lot of VRAM.
  o.flags = release ? sl::DLSSGFlags{} : sl::DLSSGFlags::eRetainResourcesWhenOff;
  sl::ViewportHandle vp{0};
  sl::Result r = p_slDLSSGSetOptions(vp, o);
  bool ok = SlSucceeded(r, "DLSS-G options");
  Log("DLSS-G %s (frames=%u%s) -> %d", on ? "ON" : "off",
      frames, release ? ", memory released" : "", (int)r);
  if (ok) {
    if (on) g_optionsError = 0;
  } else if (on) {
    g_optionsError = (int)r;
    // An unsuccessful change must not leave the previous mode generating while the UI says off.
    o.mode = sl::DLSSGMode::eOff;
    p_slDLSSGSetOptions(vp, o);
  }
  return ok;
}

uint32_t SlFramesMax() { return g_framesMax; }

static uint32_t g_lastStatus = 0;
static float g_ratio = 1.0f;
static uint64_t g_fgVram = 0;
SlFgState SlGetFgState() {
  SlFgState s;
  s.status = g_lastStatus;
  s.framesMax = g_framesMax;
  s.ratio = g_ratio;
  s.optionsError = g_optionsError;
  s.fgVram = g_fgVram;
  s.srVram = 0;
  if (p_slDLSSGetState) {
    sl::DLSSState ds{};
    sl::ViewportHandle vp{0};
    if (p_slDLSSGetState(vp, ds) == sl::Result::eOk) s.srVram = ds.estimatedVRAMUsageInBytes;
  }
  uint64_t t = g_vramWarnTick.load();
  s.overBudget = t && GetTickCount64() - t < 5000;
  return s;
}

void SlPollState() {
  if (!p_slDLSSGGetState) return;
  sl::DLSSGState st{};
  sl::ViewportHandle vp{0};
  if (!SlSucceeded(p_slDLSSGGetState(vp, st, nullptr), "DLSS-G state")) return;
  bool changed = g_framesMax != st.numFramesToGenerateMax;
  g_framesMax = st.numFramesToGenerateMax;
  static uint32_t last = 0xFFFFFFFF;
  if ((uint32_t)st.status != last || changed) {
    last = (uint32_t)st.status;
    Log("DLSS-G status=%u framesMax=%u vsyncSupport=%u", (unsigned)st.status,
        st.numFramesToGenerateMax, (unsigned)st.bIsVsyncSupportAvailable);
  }
  if ((uint32_t)st.status) LogEvent("FG-status", "DLSS-G status=%u maxGenerated=%u", (unsigned)st.status, st.numFramesToGenerateMax);
  g_lastStatus = (uint32_t)st.status;
  g_fgVram = st.estimatedVRAMUsageInBytes;
  g_ratio += 0.03f * ((float)st.numFramesActuallyPresented - g_ratio);
  static uint32_t polls = 0, shown = 0;
  polls++;
  shown += st.numFramesActuallyPresented;
  if (polls >= 600) {
    Log("FG stats: %u presents -> %u frames displayed (x%.2f)", polls, shown, shown / (double)polls);
    polls = shown = 0;
  }
}
}  // namespace feverscaler
