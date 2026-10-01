// Render resolution for DLSS, decoupled from the game's Resolution Scale slider.
//
// TF3 (build 40408) sizes its views in one renderer function (0x1402FB630):
//   resize(renderer, const ViewConfig* {width, height, ?, scale})
// It stores the config at renderer + 0x78 and renders at width x height x scale. The world view's
// renderer is resized
//   - to the window size with the Resolution Scale setting (settings + 0x7C; settings pointer read by
//     the getter at 0x1402C6A30) by the callers that size it to the window,
//   - to its stored size with the new setting when graphics settings are applied (0x140E72642),
//   - to 0x0 and back to its render size at scale 1.0, and up and back at scale 1.0 for screenshots.
// While a DLSS preset is active every window-size resize of the world view gets the preset's scale,
// so the slider has no effect. A preset change repeats the game's last window-size resize on the
// window thread, the thread the game resizes from itself.
#include "game_scale.h"

#include <windows.h>
#include <intrin.h>

#include <MinHook.h>

#include <atomic>
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
};
std::mutex g_callMx;
Call g_main;  // the game's last window-size resize of the world view
thread_local bool t_ours = false;  // inside a resize we issued ourselves

// The callers that size views to the window load the Resolution Scale setting right before the call
// with `vmovss xmm0, [rax+7Ch]` (C5 FA 10 40 7C) from the settings object.
bool FromSettingsScale(const uint8_t* ret) {
  static const uint8_t kLoad[] = {0xC5, 0xFA, 0x10, 0x40, 0x7C};
  for (const uint8_t* p = ret - 48; p + sizeof(kLoad) <= ret; ++p)
    if (!memcmp(p, kLoad, sizeof(kLoad))) return true;
  return false;
}

float SliderScale() {
  if (!g_settingsPtr || !*g_settingsPtr) return 0.0f;
  float s;
  memcpy(&s, *g_settingsPtr + kSettingsScaleOffset, 4);
  return (s >= 0.1f && s <= 4.0f) ? s : 0.0f;
}

void __fastcall h_Resize(void* renderer, ViewConfig* cfg) {
  const uint8_t* ret = (const uint8_t*)_ReturnAddress();
  const uint8_t* base = (const uint8_t*)GetModuleHandleW(nullptr);
  if (!cfg || t_ours) {
    o_Resize(renderer, cfg);
    return;
  }
  float o = g_override.load();
  VkExtent2D se = SwapchainExtent();
  bool windowSize = cfg->width > 0 && cfg->height > 0 &&
                    (!se.width || (cfg->width == (int32_t)se.width && cfg->height == (int32_t)se.height));
  bool settingsScale = FromSettingsScale(ret);
  void* world;
  {
    std::lock_guard<std::mutex> lk(g_callMx);
    world = g_main.renderer;
  }
  if (settingsScale && !windowSize) {
    // Previews in game windows (vehicle, station) also take the Resolution Scale; only the view
    // that fills the window is the world view DLSS upscales. Previews get no DLSS: full resolution
    // or the preset's render scale (menu).
    if (o > 0.0f && cfg->width > 0) {
      ViewConfig c = *cfg;
      c.scale = Cfg().previewScale == 1 ? o : 1.0f;
      Log("game resize %dx%d (preview): scale %.3f", c.width, c.height, c.scale);
      o_Resize(renderer, &c);
      return;
    }
    o_Resize(renderer, cfg);
    return;
  }
  if (windowSize && (settingsScale || renderer == world)) {
    g_resizeThread = GetCurrentThreadId();
    {
      std::lock_guard<std::mutex> lk(g_callMx);
      g_main = Call{renderer, *cfg, true};
    }
    if (o > 0.0f) {
      ViewConfig c = *cfg;
      c.scale = o;
      Log("game resize %dx%d: render scale %.3f instead of %.3f", c.width, c.height, o, cfg->scale);
      o_Resize(renderer, &c);
      return;
    }
    Log("game resize %dx%d: scale %.3f (game)", cfg->width, cfg->height, cfg->scale);
    o_Resize(renderer, cfg);
    return;
  }
  Log("game resize %dx%d: scale %.3f left as is (caller +%#llx)", cfg->width, cfg->height, cfg->scale,
      (unsigned long long)(ret - base));
  o_Resize(renderer, cfg);
}

// Window thread: repeat the game's last window-size resize of the world view with the current scale.
void ResizeOnWindowThread() {
  // Only while the world view is being drawn: its renderer is then certainly alive. Otherwise the
  // game's own next resize (e.g. loading a save) applies the preset.
  if (GetTickCount64() - LastWorldPassTick() > 500) {
    Log("render scale: no world view on screen - applies at the game's next resize");
    return;
  }
  Call mainCall;
  {
    std::lock_guard<std::mutex> lk(g_callMx);
    mainCall = g_main;
  }
  if (!mainCall.valid) return;
  float o = g_override.load();
  ViewConfig c = mainCall.cfg;
  if (o > 0.0f) c.scale = o;
  Log("resize for DLSS preset: %dx%d at scale %.3f", c.width, c.height, c.scale);
  t_ours = true;
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
