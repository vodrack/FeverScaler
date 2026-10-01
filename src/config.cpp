#include "config.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "log.h"

namespace feverscaler {
static Config g_cfg;
static std::wstring g_ini;
const Config& Cfg() { return g_cfg; }

void SaveGeneratedFrames(uint32_t frames) {
  g_cfg.generatedFrames = frames;
  if (g_ini.empty()) return;
  wchar_t v[16];
  swprintf(v, 16, L"%u", frames);
  WritePrivateProfileStringW(L"feverscaler", L"GeneratedFrames", v, g_ini.c_str());
}

void SaveFrameGeneration(bool on) {
  g_cfg.frameGeneration = on;
  if (!g_ini.empty()) WritePrivateProfileStringW(L"feverscaler", L"FrameGeneration", on ? L"1" : L"0", g_ini.c_str());
}

float DlssModeScale(uint32_t mode) {
  static const float k[kDlssModeCount] = {0.0f, 1.0f, 2.0f / 3.0f, 0.58f, 0.5f, 1.0f / 3.0f};
  return mode < kDlssModeCount ? k[mode] : 0.0f;
}
const char* DlssModeName(uint32_t mode) {
  static const char* k[kDlssModeCount] = {"Game slider", "DLAA", "Quality", "Balanced", "Performance", "Ultra Performance"};
  return mode < kDlssModeCount ? k[mode] : "?";
}

void SaveDlssMode(uint32_t mode) {
  g_cfg.dlssMode = mode;
  wchar_t v[16];
  swprintf(v, 16, L"%u", mode);
  if (!g_ini.empty()) WritePrivateProfileStringW(L"feverscaler", L"DlssMode", v, g_ini.c_str());
}

void SavePreviewScale(uint32_t mode) {
  g_cfg.previewScale = mode;
  wchar_t v[16];
  swprintf(v, 16, L"%u", mode);
  if (!g_ini.empty()) WritePrivateProfileStringW(L"feverscaler", L"PreviewScale", v, g_ini.c_str());
}

void SaveMenuKey(int key) {
  g_cfg.keyMenu = key;
  std::wstring v;
  if (key & kKeyCtrl) v += L"Ctrl+";
  if (key & kKeyShift) v += L"Shift+";
  if (key & kKeyAlt) v += L"Alt+";
  wchar_t code[16];
  swprintf(code, 16, L"0x%02X", key & 0xFFFF);
  if (!g_ini.empty()) WritePrivateProfileStringW(L"feverscaler", L"KeyMenu", (v + code).c_str(), g_ini.c_str());
}

std::string KeyName(int key) {
  int vkey = key & 0xFFFF;
  if (!vkey) return "Off";
  std::string mods;
  if (key & kKeyCtrl) mods += "Ctrl+";
  if (key & kKeyShift) mods += "Shift+";
  if (key & kKeyAlt) mods += "Alt+";
  if (vkey == VK_PAUSE) return mods + "Pause";
  UINT sc = MapVirtualKeyW((UINT)vkey, MAPVK_VK_TO_VSC);
  bool ext = vkey == VK_INSERT || vkey == VK_DELETE || vkey == VK_HOME || vkey == VK_END || vkey == VK_PRIOR ||
             vkey == VK_NEXT || (vkey >= VK_LEFT && vkey <= VK_DOWN) || vkey == VK_DIVIDE;
  wchar_t name[64]{};
  char utf8[256]{};
  if (sc && GetKeyNameTextW((LONG)((sc << 16) | (ext ? 1u << 24 : 0u)), name, 64) > 0 &&
      WideCharToMultiByte(CP_UTF8, 0, name, -1, utf8, sizeof(utf8), nullptr, nullptr) > 0)
    return mods + utf8;
  snprintf(utf8, sizeof(utf8), "key 0x%02X", vkey);
  return mods + utf8;
}

// PC scan codes (0xE0.. = extended) of SDL scancodes 4 to 115, which are USB HID usage IDs, i.e.
// physical keys. 0: none, or a fixed virtual key (VirtualKeyOf).
static constexpr uint16_t kPcScanCodes[] = {
    0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,  // 4: A-M
    0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C,  // 17: N-Z
    0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,                    // 30: 1-0
    0x1C, 0x01, 0x0E, 0x0F, 0x39,                                                  // 40: Enter Esc Backspace Tab Space
    0x0C, 0x0D, 0x1A, 0x1B, 0x2B, 0x2B, 0x27, 0x28, 0x29, 0x33, 0x34, 0x35, 0x3A,  // 45: - = [ ] \ # ; ' ` , . / Caps Lock
    0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x57, 0x58,        // 58: F1-F12
    0xE037, 0x46, 0, 0xE052, 0xE047, 0xE049, 0xE053, 0xE04F, 0xE051,               // 70: Print Scroll Pause Ins Home PgUp Del End PgDn
    0xE04D, 0xE04B, 0xE050, 0xE048,                                                // 79: Right Left Down Up
    0x45, 0xE035, 0x37, 0x4A, 0x4E, 0xE01C,                                        // 83: Num Lock, numpad / * - + Enter
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,                                               // 89: numpad 1-9 0 .
    0x56, 0xE05D, 0, 0,                                                            // 100: ISO \, Menu, Power, numpad =
    0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x6B, 0x6C, 0x6D, 0x6E, 0x76,        // 104: F13-F24
};
static_assert(sizeof(kPcScanCodes) / sizeof(kPcScanCodes[0]) == 115 - 4 + 1, "SDL scancodes 4 to 115");

// Virtual key of an SDL scancode in the current keyboard layout; 0 if none.
static int VirtualKeyOf(int sdl) {
  // No scan code to look up: Pause sends a sequence, and MapVirtualKey gives the numpad digits as
  // their Num Lock off keys (Home, End, ...).
  if (sdl == 72) return VK_PAUSE;
  if (sdl >= 89 && sdl <= 97) return VK_NUMPAD1 + (sdl - 89);
  if (sdl == 98) return VK_NUMPAD0;
  if (sdl == 99) return VK_DECIMAL;
  if (sdl < 4 || sdl > 115 || !kPcScanCodes[sdl - 4]) return 0;
  return (int)MapVirtualKeyW(kPcScanCodes[sdl - 4], MAPVK_VSC_TO_VK_EX);
}

int KeyFromScancode(int key) {
  int vk = VirtualKeyOf(key & 0xFFFF);
  return vk ? vk | (key & ~0xFFFF) : 0;
}

int ScancodeFromKey(int key) {
  // Searched with the forward mapping: MapVirtualKey's reverse drops the 0xE0 of Home, Insert, arrows.
  if (key & 0xFFFF)
    for (int sdl = 4; sdl <= 115; ++sdl)
      if (VirtualKeyOf(sdl) == (key & 0xFFFF)) return sdl | (key & ~0xFFFF);
  return 0;
}

void SaveDlssPreset(uint32_t preset) {
  g_cfg.dlssPreset = preset;
  wchar_t v[16];
  swprintf(v, 16, L"%u", preset);
  if (!g_ini.empty()) WritePrivateProfileStringW(L"feverscaler", L"DlssPreset", v, g_ini.c_str());
}

void SaveSuperResolution(bool on) {
  g_cfg.superResolution = on;
  if (!g_ini.empty()) WritePrivateProfileStringW(L"feverscaler", L"SuperResolution", on ? L"1" : L"0", g_ini.c_str());
}

// resolutionScale from a settings.lua of the game; 0 if the file or the value is missing.
float ReadGameResolutionScale(const wchar_t* userDataDir) {
  if (!userDataDir || !*userDataDir) return 0;
  std::wstring path = std::wstring(userDataDir) + L"settings.lua";
  FILE* f = _wfopen(path.c_str(), L"r");
  if (!f) return 0;
  float scale = 0;
  char line[256];
  while (fgets(line, sizeof(line), f))
    if (const char* p = strstr(line, "resolutionScale =")) scale = (float)atof(p + 17);
  fclose(f);
  return std::isfinite(scale) && scale >= 0.1f && scale <= 4.0f ? scale : 0;
}

static int ReadInt(const std::wstring& ini, const wchar_t* key, int def) {
  return (int)GetPrivateProfileIntW(L"feverscaler", key, def, ini.c_str());
}
static float ReadFloat(const std::wstring& ini, const wchar_t* key, float def) {
  wchar_t buf[64]{};
  GetPrivateProfileStringW(L"feverscaler", key, L"", buf, 64, ini.c_str());
  return buf[0] ? (float)_wtof(buf) : def;
}

void LoadConfig() {
  std::wstring ini = PluginDir() + L"feverscaler.ini";
  if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
    FILE* f = _wfopen(ini.c_str(), L"w");
    if (f) {
      fputs(
          "[feverscaler]\n"
          "; 0 = this plugin installs no hooks; the separately loaded RTX40MFG unlock still runs\n"
          "Enabled=1\n"
          "; DLSS Frame Generation on (1) or off (0); also switched in the menus\n"
          "FrameGeneration=1\n"
          "; generated frames per rendered frame: 1 = x2 ... 5 = x6, limited by GPU/driver support\n"
          "GeneratedFrames=1\n"
          "; Reflex: 0 off, 1 low latency, 2 low latency + boost (DLSS-G needs Reflex on)\n"
          "ReflexMode=1\n"
          "ReflexSleep=1\n"
          "; real motion vectors for moving vehicles (1) or camera motion only (0)\n"
          "ObjectMotion=1\n"
          "SkinnedMotion=1\n"
          "TagHudless=1\n"
          "OnAfterFrames=60\n"
          "OffAfterIdleFrames=30\n"
          "TeleportDistance=100\n"
          "CameraCutDistance=500\n"
          "; menu key while the game is in the foreground (default: backslash on US keyboards):\n"
          "; a virtual-key code (0x..) or a character, optionally Ctrl+ / Shift+ / Alt+ (e.g. Ctrl+Shift+F);\n"
          "; 0 = no key. Also in Settings > Graphics > FeverScaler.\n"
          "KeyMenu=0xDC\n"
          "; Streamline messages go to bounded FeverScaler logs: 0/1 warnings/errors, 2 verbose info\n"
          "SlLogLevel=1\n"
          "; DLSS Super Resolution: upscales the game's render size (Resolution Scale in the game's\n"
          "; graphics settings) to the screen; at 100 % it runs as DLAA. Also in the menu.\n"
          "SuperResolution=1\n"
          "; DLSS model (preset): 5 = CNN, 11 = transformer (DLSS 4, K), 12 = 2nd-gen transformer\n"
          "; (DLSS 4.5, L, sharpest), 13 = 2nd-gen transformer, faster (M), 0 = DLSS default\n"
          "DlssPreset=12\n"
          "; render resolution: 0 = the game's Resolution Scale slider, 1 DLAA (100 %), 2 Quality (66.7 %),\n"
          "; 3 Balanced (58 %), 4 Performance (50 %), 5 Ultra Performance (33.3 %)\n"
          "DlssMode=2\n"
          "; vehicle / station window previews (DLSS does not upscale them) while a preset is chosen:\n"
          "; 0 = full resolution, 1 = the preset's render scale, 2 = the game's Resolution Scale slider\n"
          "PreviewScale=1\n"
          "; 1 = keep the game's alpha-to-coverage with DLSS (without MSAA it renders foliage as a dot pattern)\n"
          "AlphaToCoverage=0\n"
          "; its replacement: 1 = hashed alpha test (soft foliage edges once DLSS averages it), 0 = solid cut-outs\n"
          "HashedAlpha=1\n",
          f);
      fclose(f);
    }
  }
  // Keys: a virtual-key code (0x..), or a single character, optionally with Ctrl+ / Shift+ / Alt+.
  auto hex = [&](const wchar_t* key, int def) {
    wchar_t buf[64]{};
    GetPrivateProfileStringW(L"feverscaler", key, L"", buf, 64, ini.c_str());
    std::wstring s = buf;
    if (s.empty()) return def;
    int mods = 0;
    for (size_t p; s.size() > 1 && (p = s.find(L'+')) != std::wstring::npos && p + 1 < s.size();) {
      std::wstring t = s.substr(0, p);
      if (!_wcsicmp(t.c_str(), L"ctrl")) mods |= kKeyCtrl;
      else if (!_wcsicmp(t.c_str(), L"shift")) mods |= kKeyShift;
      else if (!_wcsicmp(t.c_str(), L"alt")) mods |= kKeyAlt;
      else break;
      s = s.substr(p + 1);
    }
    if (!_wcsicmp(s.c_str(), L"none") || !_wcsicmp(s.c_str(), L"off")) return 0;  // no key
    int vk;
    if (s.size() == 1 && !iswdigit(s[0])) {
      SHORT r = VkKeyScanW(s[0]);
      vk = r == -1 ? 0 : (r & 0xFF);
    } else {
      wchar_t* end = nullptr;
      vk = (int)wcstol(s.c_str(), &end, 0);
      if (!vk && end && end != s.c_str() && !*end) return 0;  // "0": no key
    }
    return vk ? (vk | mods) : def;
  };
  Config c;
  c.enabled = ReadInt(ini, L"Enabled", 1) != 0;
  c.frameGeneration = ReadInt(ini, L"FrameGeneration", 1) != 0;
  int frames = ReadInt(ini, L"GeneratedFrames", 1);
  c.generatedFrames = frames < 1 ? 1 : (uint32_t)frames;
  if (c.generatedFrames > kMaxGeneratedFrames) c.generatedFrames = kMaxGeneratedFrames;
  c.reflexMode = ReadInt(ini, L"ReflexMode", 1);
  c.reflexSleep = ReadInt(ini, L"ReflexSleep", 1) != 0;
  c.objectMotion = ReadInt(ini, L"ObjectMotion", 1) != 0;
  c.skinnedMotion = ReadInt(ini, L"SkinnedMotion", 1) != 0;
  c.tagHudless = ReadInt(ini, L"TagHudless", 1) != 0;
  c.onAfterFrames = (uint32_t)ReadInt(ini, L"OnAfterFrames", 60);
  c.offAfterIdleFrames = (uint32_t)ReadInt(ini, L"OffAfterIdleFrames", 30);
  c.teleportDistance = ReadFloat(ini, L"TeleportDistance", 100.f);
  c.cameraCutDistance = ReadFloat(ini, L"CameraCutDistance", 500.f);
  c.keyMenu = hex(L"KeyMenu", 0xDC);
  c.slLogLevel = ReadInt(ini, L"SlLogLevel", 1);
  c.superResolution = ReadInt(ini, L"SuperResolution", 1) != 0;
  c.dlssPreset = (uint32_t)ReadInt(ini, L"DlssPreset", 12);
  c.keepAlphaToCoverage = ReadInt(ini, L"AlphaToCoverage", 0) != 0;
  c.hashedAlpha = ReadInt(ini, L"HashedAlpha", 1) != 0;
  c.dlssMode = (uint32_t)ReadInt(ini, L"DlssMode", 2);
  if (c.dlssMode >= kDlssModeCount) c.dlssMode = 2;
  c.previewScale = (uint32_t)ReadInt(ini, L"PreviewScale", 1);
  if (c.previewScale >= kPreviewScaleCount) c.previewScale = 1;
  g_cfg = c;
  g_ini = ini;
  Log("config: enabled=%d frames=%u reflex=%d sleep=%d objectMotion=%d skinned=%d hudless=%d on=%u off=%u",
      c.enabled, c.generatedFrames, c.reflexMode, c.reflexSleep, c.objectMotion, c.skinnedMotion, c.tagHudless,
      c.onAfterFrames, c.offAfterIdleFrames);
}
}  // namespace feverscaler
