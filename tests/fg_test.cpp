// Exercise the real config and Streamline adapter with fake SDK entry points. Including the
// implementation keeps the test seam private; production builds have no simulation switches.
#include "../src/config.cpp"
#include "../src/sl_bridge.cpp"
#include "../src/sr_policy.h"

#include <cmath>
#include <cstdlib>
#include <cstdarg>
#include <set>
#include <string>
#include <vector>

namespace feverscaler {
static std::wstring testDir;
const std::wstring& PluginDir() { return testDir; }
const std::wstring& GameDir() { return testDir; }
void Log(const char*, ...) {}
static std::string lastEventKey;
static unsigned eventCount = 0;
void LogEvent(const char* key, const char*, ...) { lastEventKey = key; ++eventCount; }
void LogFlushRepeats() {}
}  // namespace feverscaler

using namespace feverscaler;
#define CHECK(condition) do { if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); std::exit(1); } } while (0)

struct Applied {
  sl::DLSSGMode mode;
  uint32_t frames;
  float target;
  sl::DLSSGFlags flags;
};
static std::vector<Applied> calls;
static std::vector<sl::Result> results;
static uint32_t maxFrames = 5;
static sl::Boolean dynamicSupport = sl::eFalse;
static sl::Result stateResult = sl::Result::eOk;

static sl::Result SetOptions(const sl::ViewportHandle&, const sl::DLSSGOptions& o) {
  calls.push_back({o.mode, o.numFramesToGenerate, o.dynamicTargetFrameRate, o.flags});
  if (results.empty()) return sl::Result::eOk;
  auto result = results.front();
  results.erase(results.begin());
  return result;
}
static sl::Result GetState(const sl::ViewportHandle&, sl::DLSSGState& s, const sl::DLSSGOptions*) {
  s.numFramesToGenerateMax = maxFrames;
  s.bIsDynamicMFGSupported = dynamicSupport;
  s.numFramesActuallyPresented = 1;
  return stateResult;
}
// Streamline as it looks after device creation: `missing` features have no functions, and
// slIsFeatureSupported answers `unsupported` for them.
static std::set<sl::Feature> missing;
static sl::Result unsupported = sl::Result::eErrorFeatureNotSupported;
struct FakeToken : sl::FrameToken {
  operator uint32_t() const override { return 7; }
};
static FakeToken token;
static sl::Result FakeReflexOptions(const sl::ReflexOptions&) { return sl::Result::eOk; }
static sl::Result FakeEvaluate(sl::Feature, const sl::FrameToken&, const sl::BaseStructure**, uint32_t, sl::CommandBuffer*) {
  return sl::Result::eOk;
}
static sl::Result FakeFunction(sl::Feature feature, const char* name, void*& fn) {
  static int any;  // functions other than Reflex options are only checked for presence, never called
  fn = missing.count(feature) ? nullptr : !strcmp(name, "slReflexSetOptions") ? (void*)FakeReflexOptions : (void*)&any;
  return fn ? sl::Result::eOk : sl::Result::eErrorFeatureNotSupported;
}
static sl::Result FakeSupported(sl::Feature feature, const sl::AdapterInfo& ai) {
  CHECK(ai.vkPhysicalDevice == (void*)0x1234);
  return missing.count(feature) ? unsupported : sl::Result::eOk;
}
static sl::Result FakeNewToken(sl::FrameToken*& t, const uint32_t*) {
  t = &token;
  return sl::Result::eOk;
}
static void StartStreamline(std::set<sl::Feature> without, sl::Result why) {
  missing = without;
  unsupported = why;
  g_core = g_ready = false;
  g_token = nullptr;
  p_slGetFeatureFunction = FakeFunction;
  p_slIsFeatureSupported = FakeSupported;
  p_slGetNewFrameToken = FakeNewToken;
  p_slEvaluateFeature = FakeEvaluate;
  SlOnDeviceCreated((VkPhysicalDevice)0x1234);
}
static bool Contains(const char* text, const char* part) { return std::strstr(text, part) != nullptr; }

static void Reset() {
  g_cfg = Config{};
  calls.clear();
  results.clear();
  g_framesMax = 0;
  g_optionsError = 0;
  stateResult = sl::Result::eOk;
  dynamicSupport = sl::eFalse;
  maxFrames = 5;
  p_slDLSSGSetOptions = SetOptions;
  p_slDLSSGGetState = GetState;
}

int main() {
  StreamlineLog(sl::LogType::eInfo, "ordinary info");
  CHECK(eventCount == 0);
  StreamlineLog(sl::LogType::eWarn, "[19-00-01][warn][tid:1]dlfg.cpp:691[processFrame] Pacer timeout frame 10");
  CHECK(eventCount == 1);
  auto firstKey = lastEventKey;
  StreamlineLog(sl::LogType::eWarn, "[19-00-02][warn][tid:7]dlfg.cpp:691[processFrame] Pacer timeout frame 20");
  CHECK(eventCount == 2 && lastEventKey == firstKey);
  StreamlineLog(sl::LogType::eError, "[error]vulkan.cpp:1615[waitCPUFence] timeout");
  CHECK(eventCount == 3 && lastEventKey != firstKey);
  wchar_t tmp[MAX_PATH], leaf[96];
  CHECK(GetTempPathW(MAX_PATH, tmp));
  swprintf(leaf, 96, L"feverscaler-test-%lu-%llu\\", GetCurrentProcessId(), GetTickCount64());
  testDir = std::wstring(tmp) + leaf;
  CHECK(CreateDirectoryW(testDir.c_str(), nullptr));
  const auto ini = testDir + L"feverscaler.ini";
  LoadConfig();
  CHECK(Cfg().generatedFrames == 1);
  // New configs omit the removed options; older configs may still contain them.
  wchar_t value[32];
  CHECK(GetPrivateProfileStringW(L"feverscaler", L"FrameGenerationMode", L"", value, 32, ini.c_str()) == 0);
  CHECK(GetPrivateProfileStringW(L"feverscaler", L"DynamicTargetFPS", L"", value, 32, ini.c_str()) == 0);
  SaveGeneratedFrames(5);
  WritePrivateProfileStringW(L"feverscaler", L"FrameGenerationMode", L"1", ini.c_str());
  WritePrivateProfileStringW(L"feverscaler", L"DynamicTargetFPS", L"143", ini.c_str());
  LoadConfig();
  CHECK(Cfg().generatedFrames == 5);
  p_slDLSSGSetOptions = SetOptions;
  g_framesMax = 5;
  CHECK(SlSetGeneration(true, Cfg().generatedFrames));
  CHECK(calls.back().mode == sl::DLSSGMode::eOn && calls.back().frames == 5 && calls.back().target == 0);
  WritePrivateProfileStringW(L"feverscaler", L"GeneratedFrames", L"-1", ini.c_str());
  LoadConfig();
  CHECK(Cfg().generatedFrames == 1);
  WritePrivateProfileStringW(L"feverscaler", L"GeneratedFrames", L"999", ini.c_str());
  LoadConfig();
  CHECK(Cfg().generatedFrames == 5);

  Reset();
  CHECK(SlSetGeneration(true, 5));  // unknown capability is never treated as x6 support
  CHECK(calls.back().frames == 1);
  for (uint32_t cap = 1; cap <= 5; ++cap) {
    maxFrames = cap;
    SlPollState();
    CHECK(SlSetGeneration(true, 5));
    CHECK(calls.back().frames == cap && calls.back().mode == sl::DLSSGMode::eOn);
  }
  maxFrames = 99;
  SlPollState();
  CHECK(SlSetGeneration(true, 99) && calls.back().frames == 5);
  CHECK(SlSetGeneration(true, 0) && calls.back().frames == 1);
  maxFrames = 0;
  SlPollState();
  CHECK(SlSetGeneration(true, 5) && calls.back().frames == 1);

  Reset();
  // Fixed generation is used regardless of the SDK's Dynamic capability flag.
  for (auto support : {sl::eFalse, sl::eInvalid, sl::eTrue}) {
    dynamicSupport = support;
    stateResult = sl::Result::eWarnOutOfVRAM;
    SlPollState();
    CHECK(SlSetGeneration(true, 5));
    CHECK(calls.back().mode == sl::DLSSGMode::eOn && calls.back().frames == 5 && calls.back().target == 0);
  }
  CHECK(SlSetGeneration(false, 5));
  CHECK(calls.back().mode == sl::DLSSGMode::eOff);
  CHECK(calls.back().flags == sl::DLSSGFlags::eRetainResourcesWhenOff);
  CHECK(SlSetGeneration(false, 5, true) && calls.back().flags == sl::DLSSGFlags{});

  calls.clear();
  results = {sl::Result::eErrorInvalidParameter};
  CHECK(!SlSetGeneration(true, 5));
  CHECK(calls.size() == 2 && calls.back().mode == sl::DLSSGMode::eOff);
  CHECK(SlGetFgState().optionsError == (int)sl::Result::eErrorInvalidParameter);
  results = {sl::Result::eWarnOutOfVRAM};
  CHECK(SlSetGeneration(true, 5) && !SlGetFgState().optionsError);
  p_slDLSSGSetOptions = nullptr;
  CHECK(!SlSetGeneration(true, 5));

  // Dev menu key: 0 / none / off mean no key; text that is not a key keeps the default.
  for (const wchar_t* off : {L"0", L"none", L"Ctrl+0"}) {
    WritePrivateProfileStringW(L"feverscaler", L"KeyMenu", off, ini.c_str());
    LoadConfig();
    CHECK(Cfg().keyMenu == 0);
  }
  WritePrivateProfileStringW(L"feverscaler", L"KeyMenu", L"xyz", ini.c_str());
  LoadConfig();
  CHECK(Cfg().keyMenu == 0xDC);
  // A key chosen in the settings page is saved in the ini's format, modifiers included.
  SaveMenuKey(0x47 | kKeyCtrl | kKeyShift);
  LoadConfig();
  CHECK(Cfg().keyMenu == (0x47 | kKeyCtrl | kKeyShift));
  SaveMenuKey(0);
  LoadConfig();
  CHECK(Cfg().keyMenu == 0);
  CHECK(KeyName(0) == "Off" && KeyName(VK_PAUSE | kKeyCtrl | kKeyAlt) == "Ctrl+Alt+Pause");
  // The settings page binds keys by SDL scancode. Layout-independent keys map to fixed virtual
  // keys, including the extended ones and the numpad digits; every key maps back to itself.
  CHECK(KeyFromScancode(65 | kKeyCtrl | kKeyShift) == (VK_F8 | kKeyCtrl | kKeyShift));
  CHECK(KeyFromScancode(74) == VK_HOME && KeyFromScancode(95) == VK_NUMPAD7 && KeyFromScancode(72) == VK_PAUSE);
  CHECK(KeyFromScancode(84) == VK_DIVIDE && KeyFromScancode(85) == VK_MULTIPLY && KeyFromScancode(79) == VK_RIGHT);
  CHECK(KeyFromScancode(0) == 0 && KeyFromScancode(102) == 0 && KeyFromScancode(300) == 0);
  for (int sdl = 4; sdl <= 115; ++sdl)
    if (int vk = KeyFromScancode(sdl | kKeyAlt)) CHECK(KeyFromScancode(ScancodeFromKey(vk)) == vk);
  CHECK(ScancodeFromKey(VK_F8 | kKeyCtrl) == (65 | kKeyCtrl) && ScancodeFromKey(VK_NUMPAD7) == 95);
  CHECK(ScancodeFromKey(VK_HOME) == 74 && ScancodeFromKey(VK_RETURN) == 40);  // not numpad 7 / numpad Enter
  CHECK(ScancodeFromKey(0) == 0 && ScancodeFromKey(VK_LBUTTON) == 0);

  // Use the actual game's userdata directory and reject missing or corrupt scale values.
  const std::wstring userData = testDir + L"userdata\\";
  CHECK(CreateDirectoryW(userData.c_str(), nullptr));
  auto writeSettings = [](const std::wstring& dir, const char* scale) {
    FILE* f = _wfopen((dir + L"settings.lua").c_str(), L"w");
    CHECK(f);
    std::fprintf(f, "function data()\nreturn { graphics = { resolutionScale = %s, }, }\nend\n", scale);
    std::fclose(f);
  };
  CHECK(ReadGameResolutionScale(L"") == 0);
  CHECK(ReadGameResolutionScale(userData.c_str()) == 0);
  writeSettings(userData, "0.5"); CHECK(ReadGameResolutionScale(userData.c_str()) == 0.5f);
  writeSettings(userData, "1.0"); CHECK(ReadGameResolutionScale(userData.c_str()) == 1.0f);
  for (const char* value : {"nan", "inf", "0", "-1", "100", "broken"}) {
    writeSettings(userData, value); CHECK(ReadGameResolutionScale(userData.c_str()) == 0);
  }
  // Key labels must follow Windows on both layouts, without changing the user's system layout.
  HKL originalLayout = GetKeyboardLayout(0);
  for (const wchar_t* layout : {L"00000409", L"00000407"}) {
    HKL loaded = LoadKeyboardLayoutW(layout, 0);
    CHECK(loaded && ActivateKeyboardLayout(loaded, 0));
    wchar_t label[64]{}; char utf8[256]{};
    UINT scan = MapVirtualKeyW(VK_OEM_5, MAPVK_VK_TO_VSC);
    CHECK(GetKeyNameTextW((LONG)(scan << 16), label, 64));
    CHECK(WideCharToMultiByte(CP_UTF8, 0, label, -1, utf8, sizeof(utf8), nullptr, nullptr));
    CHECK(KeyName(VK_OEM_5) == utf8);
    CHECK(KeyName(VK_OEM_5 | kKeyCtrl) == std::string("Ctrl+") + utf8);
    CHECK(ActivateKeyboardLayout(originalLayout, 0));
    if (loaded != originalLayout) UnloadKeyboardLayout(loaded);
  }

  // Super Resolution and frame generation are ready independently, and each says why not.
  p_slReflexSetOptions = nullptr;
  missing = {};
  StartStreamline({}, sl::Result::eOk);
  CHECK(SlCoreReady() && SlFgReady() && SlDlssAvailable());
  CHECK(!*SlSrUnavailableReason() && !*SlFgUnavailableReason());
  StartStreamline({sl::kFeatureDLSS_G}, sl::Result::eErrorOSDisabledHWS);
  CHECK(SlCoreReady() && !SlFgReady() && SlDlssAvailable());
  CHECK(!*SlSrUnavailableReason() && Contains(SlFgUnavailableReason(), "GPU scheduling"));
  CHECK(SlCurrentToken() == &token);  // SR still gets frame tokens without DLSS-G
  StartStreamline({sl::kFeatureDLSS}, sl::Result::eErrorDriverOutOfDate);
  CHECK(SlFgReady() && !SlDlssAvailable() && Contains(SlSrUnavailableReason(), "driver is too old"));
  StartStreamline({sl::kFeatureDLSS, sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL},
                  sl::Result::eErrorAdapterNotSupported);
  CHECK(SlCoreReady() && !SlFgReady() && !SlDlssAvailable());
  CHECK(Contains(SlSrUnavailableReason(), "NVIDIA RTX") && Contains(SlFgUnavailableReason(), "NVIDIA RTX"));
  StartStreamline({sl::kFeatureDLSS}, sl::Result::eOk);  // supported, yet it did not start
  CHECK(Contains(SlSrUnavailableReason(), "could not be started"));
  StartStreamline({sl::kFeatureDLSS}, (sl::Result)9999);
  CHECK(Contains(SlSrUnavailableReason(), "result 9999"));

  // The DLSS preset's render size only once DLSS has produced a frame, and not after it stops.
  {
    using E = SrScalePolicy::Event;
    SrScalePolicy sp;
    for (uint32_t i = 1; i < SrScalePolicy::kFailFrames; ++i) CHECK(sp.Update(true, false, true) == E::None);
    CHECK(sp.Update(true, false, true) == E::GaveUp && !sp.PresetScaleAllowed(true));
    sp = {};  // new session
    CHECK(!sp.PresetScaleAllowed(true));  // DLSS never worked here: the game's own resolution
    CHECK(sp.Update(true, true, true) == E::Proven && sp.PresetScaleAllowed(true));
    CHECK(!sp.PresetScaleAllowed(false));
    for (int i = 0; i < 100000; ++i) CHECK(sp.Update(true, false, false) == E::None);  // menus, loading
    CHECK(sp.PresetScaleAllowed(true));
    for (uint32_t i = 1; i < SrScalePolicy::kFailFrames; ++i) CHECK(sp.Update(true, false, true) == E::None);
    CHECK(sp.Update(true, true, true) == E::None);  // a DLSS frame resets the count
    for (uint32_t i = 1; i < SrScalePolicy::kFailFrames; ++i) CHECK(sp.Update(true, false, true) == E::None);
    CHECK(sp.Update(false, false, true) == E::None);  // SR off resets it too
    for (uint32_t i = 1; i < SrScalePolicy::kFailFrames; ++i) CHECK(sp.Update(true, false, true) == E::None);
    CHECK(sp.PresetScaleAllowed(true));
    CHECK(sp.Update(true, false, true) == E::GaveUp && !sp.PresetScaleAllowed(true));
    CHECK(sp.Update(true, true, true) == E::None && !sp.PresetScaleAllowed(true));  // no back and forth
  }

  CHECK(DeleteFileW((userData + L"settings.lua").c_str()));
  CHECK(RemoveDirectoryW(userData.c_str()));
  CHECK(DeleteFileW(ini.c_str()));
  CHECK(RemoveDirectoryW(testDir.c_str()));
  std::puts("PASS: persistence, capability limits, legacy config compatibility, fixed mode, suspend/release, SDK errors, menu key, "
            "saved render scale, independent SR/FG readiness and the preset render-scale policy");
}
