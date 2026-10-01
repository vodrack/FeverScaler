// The settings script and its state file: see bridge.h.
//
// The game's user data folder is not next to the executable (Steam: userdata\<id>\<app>\local), and
// nothing tells a plugin where it is. It is taken from the game's own file access: the folder of the
// first settings.lua the game opens, corrected by the state file itself once the script writes it.
#include "bridge.h"

#include <windows.h>

#include <MinHook.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include "config.h"
#include "frame.h"
#include "log.h"

namespace feverscaler {
namespace {
using PFN_CreateFileW = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using PFN_GetFileAttributesW = DWORD(WINAPI*)(LPCWSTR);
using PFN_GetFileAttributesExW = BOOL(WINAPI*)(LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
using PFN_FindFirstFileW = HANDLE(WINAPI*)(LPCWSTR, LPWIN32_FIND_DATAW);
PFN_CreateFileW o_CreateFileW = nullptr;
PFN_GetFileAttributesW o_GetFileAttributesW = nullptr;
PFN_GetFileAttributesExW o_GetFileAttributesExW = nullptr;
PFN_FindFirstFileW o_FindFirstFileW = nullptr;

const wchar_t kStateSuffix[] = L"/feverscaler/state.lua";
const wchar_t kSettingsSuffix[] = L"/settings.lua";

std::mutex g_dirMx;
std::wstring g_stateDir;  // <user data>\feverscaler\ ; empty until known
bool g_dirFromState = false;
thread_local bool t_ours = false;  // the plugin's own file access

// Marks the plugin's own file access, which the hooks leave alone.
struct Ours {
  bool was = t_ours;
  Ours() { t_ours = true; }
  ~Ours() { t_ours = was; }
};

bool ReadAll(const std::wstring& path, std::string& bytes) {
  Ours ours;
  FILE* f = _wfopen(path.c_str(), L"rb");
  if (!f) return false;
  bytes.clear();
  char buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) bytes.append(buf, n);
  bool ok = !ferror(f);
  fclose(f);
  return ok;
}

// Replaces `path` in one step: the game may read it at any time.
bool WriteAll(const std::wstring& path, const std::string& bytes) {
  Ours ours;
  std::wstring tmp = path + L".tmp";
  FILE* f = _wfopen(tmp.c_str(), L"wb");
  if (!f) return false;
  bool ok = fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
  ok = fclose(f) == 0 && ok;
  return ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

// ---- the settings script as base-game content -----------------------------------------------------
std::wstring g_list;      // <game>\base\_content.json
std::wstring g_listCopy;  // the plugin's copy of it with the served files; empty: nothing is served
std::wstring g_content;   // <game>\base\content\feverscaler\ ...
std::wstring g_served;    // ... is scripts\feverscaler\settings\ (both with a trailing backslash)

bool HasI(const wchar_t* s, const wchar_t* part) {
  size_t n = wcslen(part);
  for (; *s; ++s)
    if (!_wcsnicmp(s, part, n)) return true;
  return false;
}

// What the game gets instead of `name`, or "" for its own file.
std::wstring Served(LPCWSTR name, const char* api) {
  if (!name || t_ours || g_listCopy.empty()) return {};
  if (!HasI(name, L"_content.json") && !HasI(name, L"feverscaler")) return {};  // nearly every file
  wchar_t full[1024];
  DWORD n = GetFullPathNameW(name, 1024, full, nullptr);
  if (!n || n >= 1024) return {};
  std::wstring served;
  size_t dir = g_content.size() - 1;  // the folder itself, or a path in it
  if (!_wcsicmp(full, g_list.c_str())) served = g_listCopy;
  else if (n >= dir && !_wcsnicmp(full, g_content.c_str(), dir) && (!full[dir] || full[dir] == L'\\'))
    served = g_served.substr(0, g_served.size() - 1) + (full + dir);
  else return {};
  FEVERSCALER_LOG_N(20, "bridge: %s %S -> %S", api, name, served.c_str());
  return served;
}

// Writes `copy`: the game's content list with the files in g_served added to its loose files.
bool CopyList(const std::wstring& copy) {
  std::string list;
  if (!ReadAll(g_list, list)) {
    Log("bridge: cannot read %S - no settings page", g_list.c_str());
    return false;
  }
  size_t key = list.find("\"files\"");
  size_t open = key == std::string::npos ? key : list.find_first_not_of(" \t\r\n:", key + 7);
  if (open == std::string::npos || list[open] != '[') {
    Log("bridge: no file list in %S - no settings page", g_list.c_str());
    return false;
  }
  std::string add;
  WIN32_FIND_DATAW fd;
  HANDLE find = FindFirstFileW((g_served + L"*").c_str(), &fd);
  if (find != INVALID_HANDLE_VALUE) {
    do {
      char file[MAX_PATH];
      if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
          WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, file, MAX_PATH, nullptr, nullptr))
        add += std::string("\"feverscaler/") + file + "\",";
    } while (FindNextFileW(find, &fd));
    FindClose(find);
  }
  if (add.empty()) {
    Log("bridge: no files in %S - no settings page", g_served.c_str());
    return false;
  }
  size_t next = list.find_first_not_of(" \t\r\n", open + 1);
  if (next != std::string::npos && list[next] == ']') add.pop_back();  // an empty list takes no comma
  list.insert(open + 1, add);
  std::string old;
  if (ReadAll(copy, old) && old == list) return true;
  if (WriteAll(copy, list)) return true;
  Log("bridge: could not write %S (error %lu) - no settings page", copy.c_str(), GetLastError());
  return false;
}

// Case-insensitive, either slash.
bool EndsWith(const wchar_t* path, size_t len, const wchar_t* suffix, size_t slen) {
  if (len < slen) return false;
  const wchar_t* p = path + len - slen;
  for (size_t i = 0; i < slen; ++i) {
    wchar_t c = p[i] == L'\\' ? L'/' : towlower(p[i]);
    if (c != suffix[i]) return false;
  }
  return true;
}

void NoteUserDataFile(const wchar_t* path, bool isState) {
  wchar_t full[1024];
  DWORD n = GetFullPathNameW(path, 1024, full, nullptr);
  if (!n || n >= 1024) return;
  std::wstring dir(full, n);
  dir.erase(dir.find_last_of(L"\\/") + 1);
  if (!isState) dir += L"feverscaler\\";
  std::lock_guard<std::mutex> lk(g_dirMx);
  if (g_dirFromState || (!isState && !g_stateDir.empty())) return;
  if (dir != g_stateDir) Log("bridge: state folder %S (%s)", dir.c_str(), isState ? "written by the settings script" : "next to the game's settings.lua");
  g_stateDir = dir;
  g_dirFromState = isState;
}

HANDLE WINAPI h_CreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition, DWORD flags,
                            HANDLE templ) {
  if (name && !t_ours) {
    size_t len = wcslen(name);
    if (len > 4 && (name[len - 1] | 0x20) == L'a') {  // "...lua": skips nearly every file the game opens
      if (EndsWith(name, len, kStateSuffix, _countof(kStateSuffix) - 1)) NoteUserDataFile(name, true);
      else if (EndsWith(name, len, kSettingsSuffix, _countof(kSettingsSuffix) - 1)) NoteUserDataFile(name, false);
    }
    // Reads only: a write would go to the game's own file.
    if (!(access & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE))) {
      std::wstring served = Served(name, "CreateFileW");
      if (!served.empty()) return o_CreateFileW(served.c_str(), access, share, sa, disposition, flags, templ);
    }
  }
  return o_CreateFileW(name, access, share, sa, disposition, flags, templ);
}

DWORD WINAPI h_GetFileAttributesW(LPCWSTR name) {
  std::wstring served = Served(name, "GetFileAttributesW");
  return o_GetFileAttributesW(served.empty() ? name : served.c_str());
}

BOOL WINAPI h_GetFileAttributesExW(LPCWSTR name, GET_FILEEX_INFO_LEVELS level, LPVOID info) {
  std::wstring served = Served(name, "GetFileAttributesExW");
  return o_GetFileAttributesExW(served.empty() ? name : served.c_str(), level, info);
}

HANDLE WINAPI h_FindFirstFileW(LPCWSTR name, LPWIN32_FIND_DATAW data) {
  std::wstring served = Served(name, "FindFirstFileW");
  return o_FindFirstFileW(served.empty() ? name : served.c_str(), data);
}

template <class F>
MH_STATUS Hook(const char* name, F detour, F* original) {
  void* target = nullptr;
  MH_STATUS s = MH_CreateHookApiEx(L"kernelbase", name, (void*)detour, (void**)original, &target);
  return s == MH_OK ? MH_EnableHook(target) : s;
}

// ---- state file ------------------------------------------------------------------------------------
// The switches the settings page shows. Info fields are the plugin's to report, not the script's to set.
struct State {
  bool fg = false;
  bool fgAvailable = false;  // info: DLSS-G can run on this system
  uint32_t frames = 1;     // generated frames per rendered frame: 1 = x2
  uint32_t maxFrames = 1;  // info
  bool sr = false;
  bool srAvailable = false;   // info
  bool scaleControl = false;  // info: the DLSS preset can set the render resolution on this game build
  uint32_t mode = 0;          // Config::dlssMode
  uint32_t preset = 0;        // Config::dlssPreset
  uint32_t previews = 0;      // Config::previewScale
  int menuKey = 0;            // Config::keyMenu as an SDL scancode, the way the game's key events report keys
  std::string srReason, fgReason;  // info: why SR / DLSS-G cannot run ("" while they can)
  bool operator==(const State& o) const {
    return fg == o.fg && frames == o.frames && maxFrames == o.maxFrames && sr == o.sr && srAvailable == o.srAvailable &&
           scaleControl == o.scaleControl && mode == o.mode && preset == o.preset && previews == o.previews &&
           fgAvailable == o.fgAvailable && srReason == o.srReason && fgReason == o.fgReason && menuKey == o.menuKey;
  }
};

State Current() {
  FgStatus fg = FgGetStatus();
  SrStatus sr = SrGetStatus();
  State s;
  s.fg = fg.userEnabled;
  s.fgAvailable = fg.available;
  s.fgReason = fg.unavailableReason;
  s.frames = fg.generatedFrames;
  s.maxFrames = fg.maxGeneratedFrames;
  s.sr = sr.wanted;
  s.srAvailable = sr.available;
  s.srReason = sr.unavailableReason;
  s.scaleControl = sr.scaleControl;
  s.mode = sr.renderMode;
  s.preset = sr.preset;
  s.previews = Cfg().previewScale;
  s.menuKey = ScancodeFromKey(Cfg().keyMenu);
  return s;
}

struct Stamp {
  FILETIME time{};
  uint64_t size = 0;
  bool exists = false;
  bool operator==(const Stamp& o) const {
    return exists == o.exists && size == o.size && !CompareFileTime(&time, &o.time);
  }
};

Stamp StampOf(const std::wstring& path) {
  Stamp s;
  WIN32_FILE_ATTRIBUTE_DATA d;
  if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) return s;
  s.exists = true;
  s.time = d.ftLastWriteTime;
  s.size = ((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
  return s;
}

// A Lua string literal for an info text.
std::string Quote(const std::string& text) {
  std::string q = "\"";
  for (char c : text) {
    if (c == '"' || c == '\\') q += '\\';
    if ((unsigned char)c >= 0x20) q += c;
  }
  return q + "\"";
}

// The format app.loadUserdata reads and app.saveUserdata writes: a data() function returning a table.
std::string Format(const State& s) {
  std::string t = "function data()\nreturn {\n";
  auto add = [&t](const char* key, const std::string& value) { t += std::string("\t") + key + " = " + value + ",\n"; };
  auto b = [](bool v) { return std::string(v ? "true" : "false"); };
  add("fg", b(s.fg));
  add("frames", std::to_string(s.frames));
  add("maxFrames", std::to_string(s.maxFrames));
  add("sr", b(s.sr));
  add("srAvailable", b(s.srAvailable));
  add("scaleControl", b(s.scaleControl));
  add("mode", std::to_string(s.mode));
  add("preset", std::to_string(s.preset));
  add("previews", std::to_string(s.previews));
  add("fgAvailable", b(s.fgAvailable));
  add("srReason", Quote(s.srReason));
  add("fgReason", Quote(s.fgReason));
  add("menuKey", std::to_string(s.menuKey));
  return t + "}\nend\n";
}

// Reads `key = value,` lines into `s` (keys it does not find keep their value). False: could not
// open the file (the game may still be writing it).
bool Read(const std::wstring& path, State& s) {
  Ours ours;
  FILE* f = _wfopen(path.c_str(), L"rb");
  if (!f) return false;
  char line[256];
  while (fgets(line, sizeof(line), f)) {
    char key[32], val[32];
    if (sscanf(line, " %31[A-Za-z] = %31[A-Za-z0-9.]", key, val) != 2) continue;
    bool flag = !strcmp(val, "true");
    uint32_t num = (uint32_t)strtoul(val, nullptr, 10);
    if (!strcmp(key, "fg")) s.fg = flag;
    else if (!strcmp(key, "frames")) s.frames = num;
    else if (!strcmp(key, "sr")) s.sr = flag;
    else if (!strcmp(key, "mode")) s.mode = num;
    else if (!strcmp(key, "preset")) s.preset = num;
    else if (!strcmp(key, "previews")) s.previews = num;
    else if (!strcmp(key, "menuKey")) s.menuKey = (int)num;
  }
  fclose(f);
  return true;
}

// Only what the script changed since the plugin's last write: everything else in the file is what
// the plugin published, which may be older than its state by now (a change in the dev menu in between).
void Apply(const State& file, const State& published) {
  if (file.fg != published.fg) FgSetUserEnabled(file.fg);
  if (file.frames != published.frames) FgSetGeneratedFrames(file.frames);
  if (file.sr != published.sr) SrSetEnabled(file.sr);
  if (file.mode != published.mode) SrSetMode(file.mode);
  if (file.preset != published.preset && (file.preset == 5 || (file.preset >= 11 && file.preset <= 13))) SrSetPreset(file.preset);
  if (file.previews != published.previews && file.previews < kPreviewScaleCount) SavePreviewScale(file.previews);
  if (file.menuKey != published.menuKey)
    if (int key = KeyFromScancode(file.menuKey)) SaveMenuKey(key);
}
}  // namespace

void BridgeInit() {
  g_list = GameDir() + L"base\\_content.json";
  g_content = GameDir() + L"base\\content\\feverscaler\\";
  g_served = PluginDir() + L"feverscaler\\settings\\";
  std::wstring copy = PluginDir() + L"feverscaler\\base_content.json";
  if (CopyList(copy)) g_listCopy = copy;
  MH_STATUS i = MH_Initialize();
  if (i != MH_OK && i != MH_ERROR_ALREADY_INITIALIZED) return;
  MH_STATUS c = Hook("CreateFileW", h_CreateFileW, &o_CreateFileW);
  MH_STATUS a = Hook("GetFileAttributesW", h_GetFileAttributesW, &o_GetFileAttributesW);
  MH_STATUS x = Hook("GetFileAttributesExW", h_GetFileAttributesExW, &o_GetFileAttributesExW);
  MH_STATUS f = Hook("FindFirstFileW", h_FindFirstFileW, &o_FindFirstFileW);
  Log("bridge: settings page %s, file hooks %d/%d/%d/%d", g_listCopy.empty() ? "not served" : "served",
      (int)c, (int)a, (int)x, (int)f);
}

std::wstring BridgeUserDataDir() {
  std::lock_guard<std::mutex> lk(g_dirMx);
  if (g_stateDir.empty()) return {};
  std::wstring dir = g_stateDir.substr(0, g_stateDir.size() - 1);  // drop the trailing backslash
  size_t slash = dir.find_last_of(L"\\/");
  return slash == std::wstring::npos ? std::wstring() : dir.substr(0, slash + 1);
}

void BridgePoll() {
  static ULONGLONG next = 0;
  static bool started = false;
  static State published;
  static Stamp seen;
  ULONGLONG now = GetTickCount64();
  if (now < next) return;
  next = now + 100;
  std::wstring dir;
  {
    std::lock_guard<std::mutex> lk(g_dirMx);
    dir = g_stateDir;
  }
  if (dir.empty()) return;
  std::wstring path = dir + L"state.lua";

  // The file left by the last session is not a request: the first write replaces it.
  bool scriptWrote = false;
  if (started) {
    Stamp st = StampOf(path);
    scriptWrote = !(st == seen);
    if (scriptWrote && st.exists) {
      State file = published;
      if (!Read(path, file)) return;  // the game is still writing it: try again at the next poll
      Apply(file, published);
    }
  }
  // After a write by the script the file is always replaced: it then holds what the plugin made
  // of the request (a clamped value, a switch it does not offer).
  State cur = Current();
  if (started && !scriptWrote && cur == published) return;
  CreateDirectoryW(dir.c_str(), nullptr);
  if (!WriteAll(path, Format(cur))) {
    FEVERSCALER_LOG_N(3, "bridge: could not write %S (error %lu)", path.c_str(), GetLastError());
    return;
  }
  if (!started) Log("bridge: state published to %S", path.c_str());
  started = true;
  published = cur;
  seen = StampOf(path);
}
}  // namespace feverscaler
