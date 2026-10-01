// The game's VSync and MSAA while DLSS runs, decoupled from its saved settings.
//
// TF3 (build 40408) hands both settings to its Vulkan render context through virtual setters; its
// settings-apply function (0x140E71E40) calls them after settings changes:
//   slot 22 (0x142D01360): VSync (bool); the context recreates its swapchain at the next frame
//   slot 23 (0x142D00B80): VulkanRenderContext::SetDefaultNumSamplesMSAA (int); the context
//                          rebuilds its render passes and pipelines, not the world view's render
//                          targets: the settings apply resizes the world view after it (0x140E72642)
// The hooks give the context VSync off while DLSS-G is on (DLSS-G on Vulkan does not support
// VSync) and one sample while either DLSS feature is on (their inputs come from a copy of the
// scene depth, which cannot be multisampled). When a DLSS switch changes, the game's last calls are
// repeated with the new values on the window thread, the thread the game applies its settings from,
// and a new sample count is followed by the world view's resize, as in the settings apply. Without
// it the next frame begins the new render passes on the old targets and the driver crashes in
// vkCmdBeginRenderPass.
#include "game_settings.h"

#include <windows.h>

#include <MinHook.h>

#include <atomic>
#include <cstring>

#include "config.h"
#include "frame.h"
#include "game_scale.h"
#include "log.h"
#include "overlay.h"
#include "sl_bridge.h"

namespace feverscaler {
namespace {
using SetVsyncFn = void(__fastcall*)(void* ctx, bool on);
using SetMsaaFn = void(__fastcall*)(void* ctx, int samples);
using QueryContextStateFn = bool(__fastcall*)(void* ctx);

constexpr uint32_t kSetVsyncRva = 0x2D01360;
constexpr uint32_t kSetMsaaRva = 0x2D00B80;
// Virtual slot 2: a read-only context query used during renderer startup. The constructor
// initializes VSync/MSAA directly, bypassing both setters, so observe the context here too.
constexpr uint32_t kQueryContextStateRva = 0x2CFD840;
constexpr uint32_t kContextVtableRva = 0x37BC248;
const uint8_t kQueryContextStateBytes[] = {0x48, 0x8B, 0x81, 0x58, 0x03, 0x00, 0x00, 0x0F, 0xB6, 0x40, 0x3C, 0xC3};
// Up to the comparison with the context's current value (context + 0x2A1 / + 0x2DC).
const uint8_t kSetVsyncBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC,
                                  0x40, 0x0F, 0xB6, 0xF2, 0x48, 0x8B, 0xF9, 0x38, 0x91, 0xA1, 0x02, 0x00, 0x00};
const uint8_t kSetMsaaBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x48, 0x81, 0xEC, 0xA0,
                                 0x00, 0x00, 0x00, 0x48, 0x8B, 0xF9, 0x39, 0x91, 0xDC, 0x02, 0x00, 0x00};

SetVsyncFn o_SetVsync = nullptr;
SetMsaaFn o_SetMsaa = nullptr;
QueryContextStateFn o_QueryContextState = nullptr;
uintptr_t g_contextVtable = 0;
std::atomic<void*> g_ctx{nullptr};                 // the render context
std::atomic<unsigned long> g_thread{0};            // the thread the game applies its settings from
std::atomic<int> g_gameVsync{-1}, g_gameMsaa{-1};  // the game's settings; -1 until it applies them
std::atomic<int> g_vsync{-1}, g_msaa{-1};          // what the context got
std::atomic<bool> g_pending{false};

void ObserveInitialContext(void* ctx) {
  if (!ctx || g_ctx.load() == ctx || *static_cast<uintptr_t*>(ctx) != g_contextVtable) return;
  // These fields are verified by the two setters' byte signatures. Keep the initial values
  // as the user's baseline; subsequent reads must not mistake our overrides for preferences.
  auto* bytes = static_cast<uint8_t*>(ctx);
  unsigned char vsync = bytes[0x2A1];
  int samples = 0;
  memcpy(&samples, bytes + 0x2DC, sizeof(samples));
  if (vsync > 1 || samples < 1 || samples > 64 || (samples & (samples - 1))) return;
  g_gameVsync = g_vsync = vsync;
  g_gameMsaa = g_msaa = samples;
  g_thread = GetCurrentThreadId();
  g_ctx = ctx;
  Log("VSync/MSAA: initial render context observed (VSync %s, MSAA x%d)", vsync ? "on" : "off", samples);
}

bool __fastcall h_QueryContextState(void* ctx) {
  bool result = o_QueryContextState(ctx);
  ObserveInitialContext(ctx);
  return result;
}

bool FgOn() { return Cfg().frameGeneration && SlFgReady(); }
bool VsyncFor(bool game) { return game && !FgOn(); }
int MsaaFor(int game) { return FgOn() || SrRequested() ? 1 : game; }

void SetVsync(void* ctx, bool game) {
  bool v = VsyncFor(game);
  static std::atomic<int> logged{-1};
  if (logged.exchange(game * 2 + v) != game * 2 + v)
    Log("game VSync %s%s", game ? "on" : "off", v == game ? "" : " - off while DLSS Frame Generation is on");
  g_vsync = v;
  o_SetVsync(ctx, v);
}

void SetMsaa(void* ctx, int game) {
  int v = MsaaFor(game);
  static std::atomic<int> logged{-1};
  if (logged.exchange(game * 256 + v) != game * 256 + v)
    Log("game MSAA x%d%s", game, v == game ? "" : " - off while DLSS runs");
  g_msaa = v;
  o_SetMsaa(ctx, v);
}

void __fastcall h_SetVsync(void* ctx, bool on) {
  g_thread = GetCurrentThreadId();
  g_gameVsync = on;
  g_ctx = ctx;
  SetVsync(ctx, on);
}

void __fastcall h_SetMsaa(void* ctx, int samples) {
  g_thread = GetCurrentThreadId();
  g_gameMsaa = samples;
  g_ctx = ctx;
  SetMsaa(ctx, samples);
}

// Window thread: the game's last calls again, with the current DLSS switches.
void ApplyOnWindowThread() {
  void* ctx = g_ctx.load();
  int samples = g_msaa.load();
  SetVsync(ctx, g_gameVsync.load() != 0);
  SetMsaa(ctx, g_gameMsaa.load());
  if (g_msaa.load() != samples) GameScaleResizeWorld();
  g_pending = false;
}
}  // namespace

void GameSettingsInit() {
  uint8_t* base = (uint8_t*)GetModuleHandleW(nullptr);
  uint8_t* vsync = base + kSetVsyncRva;
  uint8_t* msaa = base + kSetMsaaRva;
  uint8_t* query = base + kQueryContextStateRva;
  if (memcmp(vsync, kSetVsyncBytes, sizeof(kSetVsyncBytes)) != 0 || memcmp(msaa, kSetMsaaBytes, sizeof(kSetMsaaBytes)) != 0 ||
      memcmp(query, kQueryContextStateBytes, sizeof(kQueryContextStateBytes)) != 0) {
    Log("VSync/MSAA control: game code does not match build 40408 - the game's settings apply");
    return;
  }
  MH_STATUS v = MH_CreateHook(vsync, (void*)h_SetVsync, (void**)&o_SetVsync);
  MH_STATUS m = MH_CreateHook(msaa, (void*)h_SetMsaa, (void**)&o_SetMsaa);
  if (v == MH_OK) v = MH_EnableHook(vsync);
  if (m == MH_OK) m = MH_EnableHook(msaa);
  g_contextVtable = reinterpret_cast<uintptr_t>(base + kContextVtableRva);
  MH_STATUS q = v == MH_OK && m == MH_OK ? MH_CreateHook(query, (void*)h_QueryContextState, (void**)&o_QueryContextState)
                                       : MH_ERROR_DISABLED;
  if (q == MH_OK) q = MH_EnableHook(query);
  Log("VSync/MSAA control: %s (hooks %d/%d/%d)", v == MH_OK && m == MH_OK && q == MH_OK ? "ready" : "FAILED", (int)v, (int)m, (int)q);
}

void GameSettingsPoll() {
  void* ctx = g_ctx.load();
  int vsync = g_gameVsync.load(), msaa = g_gameMsaa.load();
  // Both setters seen (the game calls them together); a hook that failed leaves its value at -1.
  if (!ctx || vsync < 0 || msaa < 0 || g_pending.load()) return;
  if ((int)VsyncFor(vsync != 0) == g_vsync.load() && MsaaFor(msaa) == g_msaa.load()) return;
  if (g_thread.load() != OverlayWindowThread()) {
    FEVERSCALER_LOG_N(1, "VSync/MSAA: the game applies its settings on thread %lu, not the window thread %lu - "
                         "applies at its next settings change", g_thread.load(), OverlayWindowThread());
    return;
  }
  g_pending = true;
  if (!OverlayRunOnWindowThread(&ApplyOnWindowThread)) g_pending = false;
}
}  // namespace feverscaler
