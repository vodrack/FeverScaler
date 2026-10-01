#include "diagnostics.h"
#include "log.h"
#include <windows.h>
#include <intrin.h>
#include <psapi.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace feverscaler {
namespace {
constexpr unsigned kSlots = 32, kFrames = 32, kStackText = 1200;
constexpr const char* kStages[] = {"vkQueuePresentKHR", "vkQueueSubmit", "prepass submit", "GPU fence wait",
                                 "DLSS evaluate", "DLSS-G options", "Reflex sleep"};
struct Slot {
  std::atomic<uint64_t> start{0}, serial{0}, stackLow{0}, stackHigh{0};
  std::atomic<uint32_t> stage{0}, thread{0};
};
std::array<Slot, kSlots> g_slots;
std::atomic<uint64_t> g_serial{0};
std::array<std::atomic<uint32_t>, 10> g_context{};
std::atomic<uint64_t> g_contextVersion{0};
thread_local bool t_busy = false;  // this thread is logging an exception or walking a stack
wchar_t g_windows[MAX_PATH];
UINT g_windowsLength = 0;

const wchar_t* BaseName(const wchar_t* path) {
  const wchar_t* slash = wcsrchr(path, L'\\');
  return slash ? slash + 1 : path;
}
// Return addresses from `c` outward, reading the stack only within [low, high).
unsigned WalkStack(CONTEXT& c, uint64_t low, uint64_t high, uint64_t* frames) {
  const bool busy = t_busy;
  t_busy = true;  // a fault on a corrupt stack ends the walk instead of being logged
  volatile unsigned n = 0;
  __try {
    while (n < kFrames && c.Rip && c.Rsp >= low && c.Rsp + 8 <= high) {
      frames[n++] = c.Rip;
      DWORD64 image = 0;
      if (PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(c.Rip, &image, nullptr)) {
        void* handler;
        DWORD64 establisher;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, c.Rip, f, &c, &handler, &establisher, nullptr);
      } else if (n == 1) {  // a leaf function, or a call to an invalid address
        c.Rip = *(const DWORD64*)c.Rsp;
        c.Rsp += 8;
      } else {
        break;
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
  t_busy = busy;
  return n;
}
// "module+0xoffset" per frame, innermost first.
void DescribeFrames(const uint64_t* frames, unsigned n, char* out, size_t size) {
  size_t used = 0;
  out[0] = 0;
  for (unsigned i = 0; i < n && used < size; ++i) {
    HMODULE module{};
    wchar_t path[MAX_PATH];
    int w;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)frames[i], &module) && GetModuleFileNameW(module, path, MAX_PATH))
      w = snprintf(out + used, size - used, "%s%S+0x%llx", i ? " " : "", BaseName(path),
          (unsigned long long)(frames[i] - (uint64_t)module));
    else
      w = snprintf(out + used, size - used, "%s0x%llx", i ? " " : "", (unsigned long long)frames[i]);
    if (w < 0) break;
    used += (size_t)w;
  }
}
// Copies a stalled thread's stack while that thread is suspended. Until it resumes only system calls
// and memcpy run here, so no lock the thread holds can block the sampler. The walk runs on the copy.
unsigned CaptureStack(uint32_t thread, uint64_t low, uint64_t high, uint64_t* frames) {
  static uint64_t copy[32768];  // 256 KiB: enough for the innermost frames of any stack
  if (thread == GetCurrentThreadId()) return 0;
  HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, thread);
  if (!h) return 0;
  CONTEXT c{};
  c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
  uint64_t size = 0;
  if (SuspendThread(h) != (DWORD)-1) {
    if (GetThreadContext(h, &c) && c.Rsp >= low && c.Rsp < high) {
      size = std::min<uint64_t>(high - c.Rsp, sizeof(copy)) & ~7ull;
      memcpy(copy, (const void*)c.Rsp, size);
    }
    ResumeThread(h);
  }
  CloseHandle(h);
  if (!size) return 0;
  // Registers and saved frame pointers into the stack now point into the copy.
  const uint64_t from = c.Rsp, delta = (uint64_t)copy - from;
  auto relocate = [&](uint64_t& v) { if (v >= from && v - from < size) v += delta; };
  static_assert(offsetof(CONTEXT, R15) - offsetof(CONTEXT, Rax) == 15 * sizeof(DWORD64), "Rax..R15 are contiguous");
  for (DWORD64* r = &c.Rax; r <= &c.R15; ++r) relocate(*r);
  for (uint64_t i = 0; i < size / 8; ++i) relocate(copy[i]);
  return WalkStack(c, (uint64_t)copy, (uint64_t)copy + size, frames);
}
// System exceptions (0xC... codes: access violations, illegal instructions, stack overflows), logged
// when raised. C++ exceptions and debugger notifications are routine and skipped.
LONG CALLBACK OnException(EXCEPTION_POINTERS* e) {
  const EXCEPTION_RECORD& x = *e->ExceptionRecord;
  if ((x.ExceptionCode >> 28) != 0xC || t_busy) return EXCEPTION_CONTINUE_SEARCH;
  t_busy = true;
  ULONG_PTR low = 0, high = 0;
  GetCurrentThreadStackLimits(&low, &high);
  CONTEXT c = *e->ContextRecord;
  uint64_t frames[kFrames];
  char stack[kStackText];
  DescribeFrames(frames, WalkStack(c, low, high, frames), stack, sizeof(stack));
  char access[64] = "";
  if (x.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && x.NumberParameters >= 2)
    snprintf(access, sizeof(access), " (%s 0x%llx)",
        x.ExceptionInformation[0] == 1 ? "write to" : x.ExceptionInformation[0] == 8 ? "execution at" : "read of",
        (unsigned long long)x.ExceptionInformation[1]);
  char key[80];
  snprintf(key, sizeof(key), "exception %.*s", (int)strcspn(stack, " "), stack);
  LogEvent(key, "exception 0x%08lX%s on thread %lu; stack: %s", x.ExceptionCode, access, GetCurrentThreadId(), stack);
  t_busy = false;
  return EXCEPTION_CONTINUE_SEARCH;
}
// Overlays, capture hooks, Vulkan layers and other mods load from outside the Windows folder.
bool ThirdParty(const wchar_t* path, size_t length) {
  return !(g_windowsLength && length > g_windowsLength && path[g_windowsLength] == L'\\' &&
           _wcsnicmp(path, g_windows, g_windowsLength) == 0);
}
struct UnicodeString {
  USHORT length, maximumLength;
  const wchar_t* buffer;
};
struct DllNotification {
  ULONG flags;
  const UnicodeString *fullName, *baseName;
  void* base;
  ULONG size;
};
using DllCallback = void(CALLBACK*)(ULONG reason, const DllNotification* data, void* context);
void CALLBACK OnDllNotification(ULONG reason, const DllNotification* data, void*) {
  constexpr ULONG kLoaded = 1;
  if (reason != kLoaded || !ThirdParty(data->fullName->buffer, data->fullName->length / sizeof(wchar_t))) return;
  wchar_t name[MAX_PATH]{};
  memcpy(name, data->baseName->buffer, std::min<size_t>(data->baseName->length, sizeof(name) - sizeof(wchar_t)));
  Log("module loaded: %S", name);
}
void Poll(uint64_t now) {
  static uint64_t reported[kSlots]{}, lastReport[kSlots]{};
  for (unsigned i = 0; i < kSlots; ++i) {
    auto& s = g_slots[i];
    uint64_t start = s.start.load(std::memory_order_acquire);
    if (!start || start == UINT64_MAX || now < start || now - start < 2000) continue;
    uint64_t serial = s.serial.load(), low = s.stackLow.load(), high = s.stackHigh.load();
    uint32_t stage = s.stage.load(), thread = s.thread.load();
    if (start != s.start.load(std::memory_order_acquire) || serial != s.serial.load()) continue;
    if (stage >= (uint32_t)DiagnosticStage::Count) continue;
    if (reported[i] == serial && now - lastReport[i] < 30000) continue;
    reported[i] = serial; lastReport[i] = now;
    uint64_t frames[kFrames];
    const unsigned n = CaptureStack(thread, low, high, frames);
    if (start != s.start.load(std::memory_order_acquire) || serial != s.serial.load()) continue;  // returned meanwhile
    char stack[kStackText];
    DescribeFrames(frames, n, stack, sizeof(stack));
    char key[96]; snprintf(key, sizeof(key), "call-stalled-%u", stage);
    LogEvent(key, "%s has not returned for %llu ms; thread=%u call=%llu (suspected stall, not a crash verdict)%s%s",
        kStages[stage], (unsigned long long)(now - start), thread, (unsigned long long)serial,
        n ? "; stack: " : "", stack);
  }
  LogFlushRepeats();
}
DWORD WINAPI Monitor(void*) {
  SetThreadDescription(GetCurrentThread(), L"FeverScaler diagnostics");
  for (;;) { Sleep(250); Poll(GetTickCount64()); }
}
}  // namespace
void DiagnosticsInit() {
  // The exception handler, load notifications and sampler thread run code from this module.
  HMODULE module{};
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
      (LPCWSTR)&DiagnosticsInit, &module)) { Log("diagnostics: unable to pin module"); return; }
  g_windowsLength = GetSystemWindowsDirectoryW(g_windows, MAX_PATH);
  if (g_windowsLength >= MAX_PATH) g_windowsLength = 0;
  if (!AddVectoredExceptionHandler(0, OnException)) Log("diagnostics: exception handler unavailable");
  // Registered before the list is taken, so no module is missed.
  using Register = LONG(NTAPI*)(ULONG flags, DllCallback callback, void* context, void** cookie);
  auto reg = (Register)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification");
  void* cookie = nullptr;
  if (!reg || reg(0, OnDllNotification, nullptr, &cookie) < 0) Log("diagnostics: module load notifications unavailable");
  HMODULE modules[512];
  DWORD bytes = 0;
  char list[1600] = "";
  size_t used = 0;
  if (K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &bytes)) {
    const DWORD count = std::min<DWORD>(bytes, sizeof(modules)) / sizeof(HMODULE);
    for (DWORD i = 0; i < count && used < sizeof(list); ++i) {
      wchar_t path[MAX_PATH];
      const DWORD length = GetModuleFileNameW(modules[i], path, MAX_PATH);
      if (!length || !ThirdParty(path, length)) continue;
      const int w = snprintf(list + used, sizeof(list) - used, "%s%S", used ? " " : "", BaseName(path));
      if (w < 0) break;
      used += (size_t)w;
    }
  }
  Log("modules from outside Windows: %s", list);
}
void DiagnosticsStart() {
  static std::atomic<bool> started{false};
  if (started.exchange(true)) return;
  HANDLE thread = CreateThread(nullptr, 0, Monitor, nullptr, 0, nullptr);
  if (thread) { CloseHandle(thread); Log("diagnostics: bounded logs, 2-second in-flight call sampler enabled"); }
  else Log("diagnostics: sampler unavailable (Windows error %lu)", GetLastError());
}
void DiagnosticsSystem(char* buffer, unsigned size) {
  RTL_OSVERSIONINFOW os{sizeof(os)};
  using GetVersion = LONG(WINAPI*)(RTL_OSVERSIONINFOW*);
  if (auto get = (GetVersion)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")) get(&os);
  char cpu[49]{};
  for (int i = 0; i < 3; ++i) {
    int registers[4];
    __cpuid(registers, 0x80000002 + i);
    memcpy(cpu + 16 * i, registers, 16);
  }
  for (size_t n = strlen(cpu); n && cpu[n - 1] == ' ';) cpu[--n] = 0;
  const char* name = cpu + strspn(cpu, " ");
  MEMORYSTATUSEX memory{sizeof(memory)};
  GlobalMemoryStatusEx(&memory);
  snprintf(buffer, size, "Windows=%lu.%lu.%lu CPU=%s threads=%lu RAM=%.0fGiB", os.dwMajorVersion,
      os.dwMinorVersion, os.dwBuildNumber, name, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS),
      memory.ullTotalPhys / 1073741824.0);
}
void DiagnosticsContext(const DiagnosticContext& c) {
  // Written only by the present thread; readers use atomic fields and a bounded sequence check.
  const uint32_t values[] = {c.fgOn,c.multiplier,c.srOn,c.srMode,c.srPreset,c.sceneValid,
                             c.renderW,c.renderH,c.outputW,c.outputH};
  bool changed = false;
  for (unsigned i = 0; i < 10; ++i) if (g_context[i].load() != values[i]) { changed = true; break; }
  if (!changed) return;
  g_contextVersion.fetch_add(1);
  for (unsigned i = 0; i < 10; ++i) g_context[i].store(values[i]);
  g_contextVersion.fetch_add(1);
}
void DiagnosticsDescribe(char* buffer, unsigned size) {
  uint32_t v[10]{};
  bool consistent = false;
  for (unsigned attempt = 0; attempt < 3; ++attempt) {
    uint64_t before = g_contextVersion.load();
    if (before & 1) continue;
    for (unsigned i = 0; i < 10; ++i) v[i] = g_context[i].load();
    if (before == g_contextVersion.load()) { consistent = true; break; }
  }
  snprintf(buffer, size, "context%s: FG=%u x%u SR=%u mode=%u preset=%u scene=%u render=%ux%u output=%ux%u",
      consistent ? "" : " (changing)",v[0],v[1],v[2],v[3],v[4],v[5],v[6],v[7],v[8],v[9]);
}
DiagnosticScope::DiagnosticScope(DiagnosticStage stage) : stage_(stage) {
  for (unsigned i = 0; i < kSlots; ++i) {
    uint64_t empty = 0;
    if (!g_slots[i].start.compare_exchange_strong(empty, UINT64_MAX)) continue;
    slot_ = (int)i; started_ = GetTickCount64();
    ULONG_PTR low = 0, high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    g_slots[i].stage = (uint32_t)stage;
    g_slots[i].thread = GetCurrentThreadId();
    g_slots[i].stackLow = low;
    g_slots[i].stackHigh = high;
    g_slots[i].serial = ++g_serial;
    g_slots[i].start.store(started_ ? started_ : 1, std::memory_order_release);
    break;
  }
}
DiagnosticScope::~DiagnosticScope() {
  if (slot_ < 0) return;
  const uint64_t duration = GetTickCount64() - started_;
  g_slots[slot_].start.store(0, std::memory_order_release);
  if (duration >= 500) {
    char key[96]; snprintf(key, sizeof(key), "slow-call-%u", (unsigned)stage_);
    LogEvent(key, "%s returned after %llu ms", kStages[(unsigned)stage_], (unsigned long long)duration);
  }
}
#ifdef FEVERSCALER_TESTS
void DiagnosticsTestPoll(uint64_t now) { Poll(now); }
#endif
}  // namespace feverscaler
