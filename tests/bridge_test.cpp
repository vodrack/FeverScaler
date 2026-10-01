// Serve the settings script through the real file hooks: BridgeInit hooks this process's kernelbase
// file functions as it does the game's. Including the implementation keeps the test seam private.
#include "../src/bridge.cpp"

#include <cstdlib>

namespace feverscaler {
static std::wstring gameDir, pluginDir;
const std::wstring& PluginDir() { return pluginDir; }
const std::wstring& GameDir() { return gameDir; }
void Log(const char*, ...) {}
// The state file's side, not exercised here.
const Config& Cfg() { static Config c; return c; }
FgStatus FgGetStatus() { return {}; }
SrStatus SrGetStatus() { return {}; }
void FgSetUserEnabled(bool) {}
void FgSetGeneratedFrames(uint32_t) {}
void SrSetEnabled(bool) {}
void SrSetMode(uint32_t) {}
void SrSetPreset(uint32_t) {}
void SavePreviewScale(uint32_t) {}
void SaveMenuKey(int) {}
int KeyFromScancode(int key) { return key; }
int ScancodeFromKey(int key) { return key; }
}  // namespace feverscaler

using namespace feverscaler;
#define CHECK(condition) do { if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); std::exit(1); } } while (0)

static void Put(const std::wstring& path, const std::string& text) { CHECK(WriteAll(path, text)); }

// What the game reads, through the hooked CreateFileW.
static std::string GameRead(const std::wstring& path) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  CHECK(h != INVALID_HANDLE_VALUE);
  std::string text(4096, '\0');
  DWORD n = 0;
  CHECK(ReadFile(h, &text[0], (DWORD)text.size(), &n, nullptr));
  CloseHandle(h);
  return text.substr(0, n);
}

int main() {
  wchar_t tmp[MAX_PATH];
  CHECK(GetTempPathW(MAX_PATH, tmp));
  std::wstring root = std::wstring(tmp) + L"feverscaler-bridge-test-" + std::to_wstring(GetCurrentProcessId()) + L"\\";
  gameDir = root + L"game\\";
  pluginDir = gameDir + L"scripts\\";
  for (const wchar_t* dir : {L"", L"game", L"game\\base", L"game\\scripts", L"game\\scripts\\feverscaler",
                             L"game\\scripts\\feverscaler\\settings"})
    CHECK(CreateDirectoryW((root + dir).c_str(), nullptr));
  const std::string list = "{\n    \"archives\": null,\n    \"files\": [\n        \"placeholder.script.lua\"\n    ]\n}\n";
  Put(gameDir + L"base\\_content.json", list);
  Put(pluginDir + L"feverscaler\\settings\\feverscaler.script.lua", "script");
  Put(pluginDir + L"feverscaler\\settings\\settings_page.res.lua", "resource");

  BridgeInit();

  // The content list as the game reads it, spelled the game's way: the script's files are base content.
  const std::string served = GameRead(gameDir + L"base/_content.json");
  CHECK(served == "{\n    \"archives\": null,\n    \"files\": [\"feverscaler/feverscaler.script.lua\","
                  "\"feverscaler/settings_page.res.lua\",\n        \"placeholder.script.lua\"\n    ]\n}\n");
  WIN32_FILE_ATTRIBUTE_DATA info;
  CHECK(GetFileAttributesExW((gameDir + L"BASE\\_content.json").c_str(), GetFileExInfoStandard, &info));
  CHECK(info.nFileSizeLow == served.size());  // sizes match what reads return
  // The script's folder in base content is the plugin's settings folder.
  CHECK(GameRead(gameDir + L"base/content/feverscaler/settings_page.res.lua") == "resource");
  CHECK(GetFileAttributesW((gameDir + L"base\\content\\feverscaler").c_str()) & FILE_ATTRIBUTE_DIRECTORY);
  WIN32_FIND_DATAW found;
  HANDLE find = FindFirstFileW((gameDir + L"base\\content\\feverscaler\\feverscaler.*").c_str(), &found);
  CHECK(find != INVALID_HANDLE_VALUE && !wcscmp(found.cFileName, L"feverscaler.script.lua"));
  FindClose(find);
  // Everything else is the game's own: other base content, near misses, writes, the plugin's own reads.
  CHECK(GetFileAttributesW((gameDir + L"base\\content\\feverscaler2\\x.lua").c_str()) == INVALID_FILE_ATTRIBUTES);
  CHECK(GetFileAttributesW((gameDir + L"base\\content\\placeholder.script.lua").c_str()) == INVALID_FILE_ATTRIBUTES);
  HANDLE write = CreateFileW((gameDir + L"base\\_content.json").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                             OPEN_EXISTING, 0, nullptr);
  LARGE_INTEGER size;
  CHECK(write != INVALID_HANDLE_VALUE && GetFileSizeEx(write, &size) && size.QuadPart == (LONGLONG)list.size());
  CloseHandle(write);
  std::string own;
  CHECK(ReadAll(gameDir + L"base\\_content.json", own) && own == list);

  // An empty file list takes the entries without a trailing comma; no list means nothing is served.
  const std::wstring copy = root + L"copy.json";
  Put(g_list, "{\"files\": [ ]}");
  CHECK(CopyList(copy) && ReadAll(copy, own) &&
        own == "{\"files\": [\"feverscaler/feverscaler.script.lua\",\"feverscaler/settings_page.res.lua\" ]}");
  Put(g_list, "{\"files\": null}");
  CHECK(!CopyList(copy));

  CHECK(MH_DisableHook(MH_ALL_HOOKS) == MH_OK);
  for (const wchar_t* file : {L"copy.json", L"game\\base\\_content.json", L"game\\scripts\\feverscaler\\base_content.json",
                              L"game\\scripts\\feverscaler\\settings\\feverscaler.script.lua",
                              L"game\\scripts\\feverscaler\\settings\\settings_page.res.lua"})
    CHECK(DeleteFileW((root + file).c_str()));
  for (const wchar_t* dir : {L"game\\scripts\\feverscaler\\settings", L"game\\scripts\\feverscaler", L"game\\scripts",
                             L"game\\base", L"game", L""})
    CHECK(RemoveDirectoryW((root + dir).c_str()));
  std::puts("PASS: content list copy, served script folder, untouched game files and writes");
}
