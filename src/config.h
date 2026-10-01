#pragma once
#include <cstdint>
#include <string>

namespace feverscaler {
// Key values: virtual-key code in the low 16 bits, required modifiers above; 0 = no key.
constexpr int kKeyCtrl = 1 << 16, kKeyShift = 1 << 17, kKeyAlt = 1 << 18;
constexpr uint32_t kMaxGeneratedFrames = 5;  // x6; also limited by the runtime's capabilities
// feverscaler.ini next to feverscaler.asi, written with defaults on first run, read once at startup.
struct Config {
  bool enabled = true;              // master switch: false = pass-through (no Streamline)
  bool frameGeneration = true;      // the user's DLSS-G switch (dev menu, settings page)
  uint32_t generatedFrames = 1;     // fixed multiplier: 1 = x2 ... 5 = x6
  int reflexMode = 1;               // 0 off, 1 low latency, 2 low latency + boost
  bool reflexSleep = true;          // call slReflexSleep once per frame
  bool objectMotion = true;         // replay moving objects into the motion-vector buffer
  bool skinnedMotion = true;        // include skinned meshes (people, animals) in the replay
  bool tagHudless = true;           // tag the pre-UI scene colour as HUD-less
  uint32_t onAfterFrames = 60;      // frames with valid inputs before DLSS-G turns on
  uint32_t offAfterIdleFrames = 30; // presents without a 3D frame before DLSS-G suspends
  float teleportDistance = 100.0f;  // per-frame object jump (m) treated as a teleport, not motion
  float cameraCutDistance = 500.0f; // per-frame camera jump (m) that resets history
  int keyMenu = 0xDC;               // VK_OEM_5 (backslash on US layouts): show/hide the dev menu
  int slLogLevel = 1;               // bounded callback log: 0/1 warnings and errors, 2 also verbose info
  // DLSS Super Resolution / DLAA (milestone 2). The render size is the game's Resolution Scale;
  // DLSS upscales it to the output (DLAA at 100 %).
  bool superResolution = true;
  uint32_t dlssPreset = 12;         // sl::DLSSPreset: 5 E (CNN), 11 K (transformer), 12 L / 13 M (2nd gen)
  // Alpha-to-coverage without MSAA is a fixed screen-space dot pattern (foliage, grass) that DLSS
  // keeps as detail; turned off while DLSS runs, DLSS anti-aliases the cut-out edges instead.
  bool keepAlphaToCoverage = false;
  bool hashedAlpha = true;  // replacement: hashed alpha test (DLSS averages it) instead of solid cut-outs
  // DLSS preset = render resolution: 0 = the game's Resolution Scale slider, 1 DLAA (100 %),
  // 2 Quality (66.7 %), 3 Balanced (58 %), 4 Performance (50 %), 5 Ultra Performance (33.3 %).
  uint32_t dlssMode = 2;
  // Vehicle / station window previews (not upscaled by DLSS) while a preset sets the render size:
  // 0 full resolution, 1 the preset's render scale, 2 the game's Resolution Scale slider.
  uint32_t previewScale = 1;
};
const Config& Cfg();
void LoadConfig();
// Stores the multiplier chosen in game (dev menu or settings page) so the next session starts with it.
void SaveGeneratedFrames(uint32_t frames);
void SaveFrameGeneration(bool on);
void SaveSuperResolution(bool on);
void SaveDlssPreset(uint32_t preset);
void SaveDlssMode(uint32_t mode);
void SavePreviewScale(uint32_t mode);
void SaveMenuKey(int key);
// A key value as the user knows it ("Ctrl+Scroll Lock", "Off"), in UTF-8, for the current keyboard layout.
std::string KeyName(int key);
// The game's key events report SDL scancodes (physical keys); the plugin checks virtual keys.
// Both keep the modifier bits; 0 if the key has no counterpart.
int KeyFromScancode(int scancodeKey);
int ScancodeFromKey(int key);
constexpr uint32_t kPreviewScaleCount = 3;
// Render scale of a DLSS preset (0 for "game slider").
float DlssModeScale(uint32_t mode);
const char* DlssModeName(uint32_t mode);
constexpr uint32_t kDlssModeCount = 6;
// Saved Resolution Scale in the actual game user-data folder; zero if unknown or invalid.
float ReadGameResolutionScale(const wchar_t* userDataDir);
}  // namespace feverscaler
