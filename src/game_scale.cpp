// Render resolution for DLSS, decoupled from the game's Resolution Scale slider.
//
// TF3 (build 40408) sizes its views in one renderer function (0x1402FB630):
//   resize(renderer, const ViewConfig* {width, height, ?, scale})
// The main view's callers pass the Resolution Scale setting (settings + 0x7C; settings pointer read
// by the getter at 0x1402C6A30) with the output size. Right before that the game also sizes a helper
// view directly at the render size (output * slider) with scale 1.0. While a DLSS preset is active
// the hook gives the main view the preset's scale and the helper view the matching render size.
// A preset change repeats the game's last resize of both on the window thread, the thread the game
// resizes from itself.
#include "game_scale.h"

#include <windows.h>
#include <intrin.h>

#include <MinHook.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

#include "config.h"
#include "bridge.h"
#include "log.h"
#include "overlay.h"
#include "tracker.h"

namespace feverscaler {
namespace {
struct ViewConfig {
  int32_t width, height, other;
  float scale;
};
using ResizeFn = void(__fastcall*)(void* renderer, ViewConfig* cfg);

constexpr uint32_t kResizeRva = 0x2FB630;
constexpr uint32_t kSettingsGetterRva = 0x2C6A30;
constexpr uint32_t kSettingsScaleOffset = 0x7C;
const uint8_t kResizeBytes[] = {0x40, 0x56, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x38, 0x48, 0x8B, 0xF1, 0x4C, 0x8B, 0xF2};
const uint8_t kGetterBytes[] = {0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0x05};

bool g_available = false;
ResizeFn o_Resize = nullptr;
uint8_t** g_settingsPtr = nullptr;  // address of the global settings pointer
std::atomic<float> g_override{0.0f};
std::atomic<float> g_fileScale{0.0f};
std::atomic<bool> g_requested{false};
std::atomic<unsigned long> g_resizeThread{0};

struct Call {
  void* renderer = nullptr;
  ViewConfig cfg{};  // as the game passed it
  bool valid = false;
  int appliedW = 0, appliedH = 0;  // helper view: size we last gave it
};
std::mutex g_callMx;
Call g_main, g_helper;
thread_local bool t_ours = false;  // inside a resize we issued ourselves

int Scaled(int v, float s) { return (int)std::floor((float)v * s + 0.5f); }

// Only the main view's callers pass the Resolution Scale setting: right before the call they load
// it with `vmovss xmm0, [rax+7Ch]` (C5 FA 10 40 7C) from the settings object.
bool FromSettingsScale(const uint8_t* ret) {
  static const uint8_t kLoad[] = {0xC5, 0xFA, 0x10, 0x40, 0x7C};
  for (const uint8_t* p = ret - 48; p + sizeof(kLoad) <= ret; ++p)
    if (!memcmp(p, kLoad, sizeof(kLoad))) return true;
  return false;
}

// A view the game sizes to the render resolution itself (output * slider, scale 1.0).
bool IsRenderSizeView(const ViewConfig& c, const ViewConfig& mainCfg, float slider) {
  if (c.scale != 1.0f || slider <= 0.0f || c.width <= 0 || mainCfg.width <= 0) return false;
  return std::abs(c.width - Scaled(mainCfg.width, slider)) <= 1 && std::abs(c.height - Scaled(mainCfg.height, slider)) <= 1;
}

float SliderScale() {
  if (!g_settingsPtr || !*g_settingsPtr) return 0.0f;
  float s;
  memcpy(&s, *g_settingsPtr + kSettingsScaleOffset, 4);
  return (s >= 0.1f && s <= 4.0f) ? s : 0.0f;
}

void ResizeHelper(void* renderer, const ViewConfig& orig, const ViewConfig& mainCfg, float o) {
  ViewConfig c = orig;
  if (o > 0.0f) {
    c.width = Scaled(mainCfg.width, o);
    c.height = Scaled(mainCfg.height, o);
  }
  {
    std::lock_guard<std::mutex> lk(g_callMx);
    if (g_helper.renderer == renderer) {
      g_helper.appliedW = c.width;
      g_helper.appliedH = c.height;
    }
  }
  Log("game resize %dx%d (render-size view): %dx%d", orig.width, orig.height, c.width, c.height);
  o_Resize(renderer, &c);
}

void __fastcall h_Resize(void* renderer, ViewConfig* cfg) {
  const uint8_t* ret = (const uint8_t*)_ReturnAddress();
  const uint8_t* base = (const uint8_t*)GetModuleHandleW(nullptr);
  if (!cfg || t_ours) {
    o_Resize(renderer, cfg);
    return;
  }
  float o = g_override.load();
  float slider = SliderScale();
  if (FromSettingsScale(ret)) {
    // Previews in game windows (vehicle, station) also take the Resolution Scale; only the view
    // that fills the window is the world view DLSS upscales.
    VkExtent2D se = SwapchainExtent();
    bool fullWindow = !se.width || (cfg->width == (int32_t)se.width && cfg->height == (int32_t)se.height);
    if (!fullWindow) {
      if (o > 0.0f && cfg->width > 0) {
        // No DLSS on previews: their own scale (menu), not the slider the preset replaced.
        ViewConfig c = *cfg;
        uint32_t pm = Cfg().previewScale;
        c.scale = pm == 1 ? o : pm == 2 ? cfg->scale : 1.0f;
        Log("game resize %dx%d (preview): scale %.3f (%s)", c.width, c.height, c.scale,
            pm == 1 ? "preset" : pm == 2 ? "game slider" : "full resolution");
        o_Resize(renderer, &c);
        return;
      }
      o_Resize(renderer, cfg);
      return;
    }
    g_resizeThread = GetCurrentThreadId();
    Call helper;
    {
      std::lock_guard<std::mutex> lk(g_callMx);
      g_main = Call{renderer, *cfg, true};
      helper = g_helper;
    }
    if (o > 0.0f && cfg->width > 0 && cfg->height > 0) {
      ViewConfig c = *cfg;
      c.scale = o;
      Log("game resize %dx%d: render scale %.3f instead of slider %.3f", c.width, c.height, o, cfg->scale);
      o_Resize(renderer, &c);
      // The helper view was sized (just before this call) from the slider: match it to our scale.
      if (helper.valid && IsRenderSizeView(helper.cfg, *cfg, slider) &&
          (helper.appliedW != Scaled(cfg->width, o) || helper.appliedH != Scaled(cfg->height, o))) {
        t_ours = true;
        ResizeHelper(helper.renderer, helper.cfg, *cfg, o);
        t_ours = false;
      }
      return;
    }
    Log("game resize %dx%d: scale %.3f (game slider)", cfg->width, cfg->height, cfg->scale);
    o_Resize(renderer, cfg);
    return;
  }
  Call mainCall;
  {
    std::lock_guard<std::mutex> lk(g_callMx);
    mainCall = g_main;
  }
  // A render-size view: remember it (also when the main view has not been seen yet), and size it
  // to our render resolution when the main view is known.
  if (cfg->scale == 1.0f && cfg->width > 0) {
    std::lock_guard<std::mutex> lk(g_callMx);
    if (!mainCall.valid || IsRenderSizeView(*cfg, mainCall.cfg, slider)) g_helper = Call{renderer, *cfg, true, cfg->width, cfg->height};
  }
  if (o > 0.0f && mainCall.valid && IsRenderSizeView(*cfg, mainCall.cfg, slider)) {
    ResizeHelper(renderer, *cfg, mainCall.cfg, o);
    return;
  }
  Log("game resize %dx%d: scale %.3f left as is (caller +%#llx)", cfg->width, cfg->height, cfg->scale,
      (unsigned long long)(ret - base));
  o_Resize(renderer, cfg);
}

// Window thread: repeat the game's last resize of the helper and main views with the current scale.
void ResizeOnWindowThread() {
  // Only while the world view is being drawn: its renderer is then certainly alive. Otherwise the
  // game's own next resize (e.g. loading a save) applies the preset.
  if (GetTickCount64() - LastWorldPassTick() > 500) {
    Log("render scale: no world view on screen - applies at the game's next resize");
    return;
  }
  Call mainCall, helper;
  {
    std::lock_guard<std::mutex> lk(g_callMx);
    mainCall = g_main;
    helper = g_helper;
  }
  if (!mainCall.valid) return;
  float o = g_override.load();
  float slider = SliderScale();
  t_ours = true;
  if (helper.valid && IsRenderSizeView(helper.cfg, mainCall.cfg, slider)) ResizeHelper(helper.renderer, helper.cfg, mainCall.cfg, o);
  ViewConfig c = mainCall.cfg;
  if (o > 0.0f) c.scale = o;
  Log("resize for DLSS preset: %dx%d at scale %.3f", c.width, c.height, c.scale);
  o_Resize(mainCall.renderer, &c);
  t_ours = false;
}
}  // namespace

bool GameScaleInit() {
  uint8_t* base = (uint8_t*)GetModuleHandleW(nullptr);
  uint8_t* resize = base + kResizeRva;
  uint8_t* getter = base + kSettingsGetterRva;
  if (memcmp(resize, kResizeBytes, sizeof(kResizeBytes)) != 0 || memcmp(getter, kGetterBytes, sizeof(kGetterBytes)) != 0) {
    Log("render scale control: game code does not match build 40408 - using the game's Resolution Scale");
    return false;
  }
  int32_t disp;
  memcpy(&disp, getter + 7, 4);
  g_settingsPtr = (uint8_t**)(getter + 11 + disp);  // mov rax, [rip + disp] ends at getter + 11
  MH_STATUS c = MH_CreateHook(resize, (void*)h_Resize, (void**)&o_Resize);
  MH_STATUS e = c == MH_OK ? MH_EnableHook(resize) : c;
  g_available = e == MH_OK;
  Log("render scale control: %s (resize hook %d/%d, settings at %p)", g_available ? "ready" : "FAILED", (int)c, (int)e,
      (void*)g_settingsPtr);
  return g_available;
}

bool GameScaleAvailable() { return g_available; }

float GameSliderScale() {
  float live = SliderScale();
  return live > 0 ? live : g_fileScale.load();
}

void GameSetScaleOverride(float scale) {
  if (!g_available) return;
  if (scale == g_override.load()) return;
  g_override = scale;
  g_requested = true;
  Log("render scale: %s", scale > 0 ? "set by DLSS preset" : "back to the game's slider");
}

float GameScaleOverride() { return g_override.load(); }

void GameScalePoll() {
  // Other builds have no verified settings pointer. Read only the user-data folder observed
  // by the bridge, and never guess that an arbitrary small 3D pass is the world view.
  static uint64_t lastRead = 0;
  uint64_t now = GetTickCount64();
  if (!g_available && now - lastRead >= 1000) {
    lastRead = now;
    g_fileScale = ReadGameResolutionScale(BridgeUserDataDir().c_str());
  }
  if (!g_available || !g_requested.load()) return;
  bool haveMain;
  {
    std::lock_guard<std::mutex> lk(g_callMx);
    haveMain = g_main.valid;
  }
  if (!haveMain) return;  // the game has not sized its views yet: its first resize applies the scale
  g_requested = false;
  if (GetTickCount64() - LastWorldPassTick() > 500) return;  // not in the world: the next game resize applies it
  if (g_resizeThread.load() == OverlayWindowThread()) {
    OverlayRunOnWindowThread(&ResizeOnWindowThread);
  } else {
    Log("render scale: the game resizes on thread %lu, not the window thread %lu - applies at its next resize",
        g_resizeThread.load(), OverlayWindowThread());
  }
}
}  // namespace feverscaler
