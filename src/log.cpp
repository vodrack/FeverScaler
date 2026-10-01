#include "log.h"
#include "diagnostics.h"
#include "feverscaler_version.h"
#include <windows.h>
#include <share.h>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace feverscaler {
namespace {
struct File {
  FILE* handle = nullptr;
  std::wstring path;
  size_t size = 0, limit = 0;
  bool Open(const std::wstring& name, size_t cap) {
    if (handle) fclose(handle);
    handle = nullptr;
    path = name; limit = cap; size = 0;
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES &&
        !MoveFileExW(path.c_str(), (path + L".previous").c_str(), MOVEFILE_REPLACE_EXISTING)) return false;
    handle = _wfsopen(path.c_str(), L"wb", _SH_DENYWR);
    return handle != nullptr;
  }
  void Write(const char* line, const char* header) {
    if (!handle) return;
    const size_t n = strlen(line);
    if (n > limit) return;
    if (size + n > limit) {
      if (!Open(path, limit)) return;
      const size_t h = strlen(header);
      if (h + n > limit) return;
      size += fwrite(header, 1, h, handle);
    }
    size += fwrite(line, 1, n, handle);
    if (ferror(handle)) { fclose(handle); handle = nullptr; return; }
    fflush(handle);
  }
};
struct Repeat {
  uint64_t hash = 0, last = 0, suppressed = 0;
  char key[80]{}, sample[512]{};
};
struct State {
  std::mutex mutex;
  File main, events;
  std::array<Repeat, 32> repeats{};
  LARGE_INTEGER start{}, frequency{};
  char session[256]{};
  char environment[512]{};
};
// Process lifetime storage: no destructor or thread joins under the loader lock.
State* g_state = nullptr;
std::wstring g_pluginDir, g_gameDir;
std::wstring DirOf(const wchar_t* path) {
  std::wstring s(path);
  size_t p = s.find_last_of(L"\\/");
  return p == std::wstring::npos ? L".\\" : s.substr(0, p + 1);
}
void Format(char* buffer, size_t size, const char* fmt, va_list ap) {
  vsnprintf(buffer, size, fmt, ap);
  buffer[size - 1] = 0;
  for (char* p = buffer; *p; ++p) if ((unsigned char)*p < 32) *p = ' ';
}
void WriteLocked(const char* message, bool event) {
  auto& s = *g_state;
  LARGE_INTEGER now{}; QueryPerformanceCounter(&now);
  char line[2560], header[1536], context[512];
  DiagnosticsDescribe(context, sizeof(context));
  snprintf(header, sizeof(header), "%s%s\n%s\n", s.session, s.environment, context);
  snprintf(line, sizeof(line), "[%9.3f] [%5lu] %s\n",
      double(now.QuadPart - s.start.QuadPart) / s.frequency.QuadPart, GetCurrentThreadId(), message);
  s.main.Write(line, header);
  if (event) {
    s.events.Write(line, header);
    snprintf(line, sizeof(line), "  %s\n", context);
    s.events.Write(line, header);
  }
}
void SummaryLocked(Repeat& r) {
  if (!r.suppressed) return;
  char message[768];
  snprintf(message, sizeof(message), "event[%s]: %llu additional occurrences; latest: %s", r.key,
      (unsigned long long)r.suppressed, r.sample);
  WriteLocked(message, true);
  r.suppressed = 0;
}
void InitFiles(const std::wstring& directory, DWORD imageTimestamp = 0) {
  if (!g_state) g_state = new State;
  auto& s = *g_state;
  QueryPerformanceFrequency(&s.frequency);
  QueryPerformanceCounter(&s.start);
  SYSTEMTIME utc{}; GetSystemTime(&utc);
  snprintf(s.session, sizeof(s.session),
      "FeverScaler " FEVERSCALER_VERSION " build " __DATE__ " " __TIME__ " | imageTimestamp=0x%08lx | session UTC %04u-%02u-%02uT%02u:%02u:%02uZ | pid=%lu\n",
      imageTimestamp, utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond, GetCurrentProcessId());
  s.main.Open(directory + L"feverscaler.log", kLogBytes);
  s.events.Open(directory + L"feverscaler-events.log", kEventLogBytes);
  s.repeats = {};
  s.environment[0] = 0;
  s.main.Write(s.session, s.session);
  s.events.Write(s.session, s.session);
}
}  // namespace
const std::wstring& PluginDir() { return g_pluginDir; }
const std::wstring& GameDir() { return g_gameDir; }
void LogInit() {
  wchar_t path[MAX_PATH]{}; HMODULE self{};
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      (LPCWSTR)&LogInit, &self);
  GetModuleFileNameW(self, path, MAX_PATH); g_pluginDir = DirOf(path);
  GetModuleFileNameW(nullptr, path, MAX_PATH); g_gameDir = DirOf(path);
  DWORD timestamp = 0;
  if (self) {
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(self);
    if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
      auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const char*>(self) + dos->e_lfanew);
      if (nt->Signature == IMAGE_NT_SIGNATURE) timestamp = nt->FileHeader.TimeDateStamp;
    }
  }
  InitFiles(g_gameDir, timestamp);
}
void Log(const char* fmt, ...) {
  if (!g_state) return;
  char message[2048]; va_list ap; va_start(ap, fmt); Format(message, sizeof(message), fmt, ap); va_end(ap);
  std::lock_guard<std::mutex> lock(g_state->mutex);
  WriteLocked(message, false);
}
void LogEnvironment(const char* fmt, ...) {
  if (!g_state) return;
  char message[512]; va_list ap; va_start(ap, fmt); Format(message, sizeof(message), fmt, ap); va_end(ap);
  std::lock_guard<std::mutex> lock(g_state->mutex);
  snprintf(g_state->environment, sizeof(g_state->environment), "%s", message);
  WriteLocked(message, true);
}
void LogEvent(const char* key, const char* fmt, ...) {
  if (!g_state) return;
  char message[2048]; va_list ap; va_start(ap, fmt); Format(message, sizeof(message), fmt, ap); va_end(ap);
  uint64_t hash = 14695981039346656037ull;
  for (const char* p = key; *p; ++p) { hash ^= (unsigned char)*p; hash *= 1099511628211ull; }
  if (!hash) hash = 1;
  const uint64_t now = GetTickCount64();
  std::lock_guard<std::mutex> lock(g_state->mutex);
  Repeat* slot = nullptr;
  for (auto& r : g_state->repeats) if (r.hash == hash) { slot = &r; break; }
  if (!slot) {
    slot = &g_state->repeats[0];
    for (auto& r : g_state->repeats) if (!r.hash || r.last < slot->last) { slot = &r; if (!r.hash) break; }
    SummaryLocked(*slot);
    *slot = {};
    slot->hash = hash;
    snprintf(slot->key, sizeof(slot->key), "%s", key);
  } else if (now - slot->last < 30000) {
    ++slot->suppressed;
    snprintf(slot->sample, sizeof(slot->sample), "%s", message);
    return;
  }
  SummaryLocked(*slot);
  slot->last = now;
  char record[2200]; snprintf(record, sizeof(record), "event[%s]: %s", slot->key, message);
  WriteLocked(record, true);
}
void LogFlushRepeats() {
  if (!g_state) return;
  const uint64_t now = GetTickCount64();
  std::lock_guard<std::mutex> lock(g_state->mutex);
  for (auto& r : g_state->repeats) if (r.hash && now - r.last >= 30000 && r.suppressed) {
    SummaryLocked(r); r.last = now;
  }
}
#ifdef FEVERSCALER_TESTS
void LogTestInit(const std::wstring& directory) { InitFiles(directory); }
void LogTestClose() {
  std::lock_guard<std::mutex> lock(g_state->mutex);
  for (auto& r : g_state->repeats) SummaryLocked(r);
  for (File* f : {&g_state->main, &g_state->events}) { if (f->handle) fclose(f->handle); f->handle = nullptr; }
}
#endif
}  // namespace feverscaler
