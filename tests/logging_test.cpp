#include "log.h"
#include "diagnostics.h"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>
#include <cstdio>
#include <cstdlib>
using namespace feverscaler;
namespace fs = std::filesystem;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #c); std::exit(1); } } while (0)
static std::string Read(const fs::path& path) {
  std::ifstream f(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
// A handled access violation, like a driver probing memory.
static void RaiseHandledAccessViolation() {
  __try { *(volatile int*)nullptr = 1; } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
int main() {
  wchar_t temp[MAX_PATH]{}; CHECK(GetTempPathW(MAX_PATH, temp));
  fs::path root = fs::path(temp) / (L"FeverScaler-logging-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
  CHECK(fs::create_directory(root));
  const auto directory = root.wstring() + L"\\";
  LogTestInit(directory);
  DiagnosticContext context; context.fgOn = 1; context.multiplier = 5; context.renderW = 1147; context.outputW = 3440;
  DiagnosticsContext(context);
  LogEnvironment("GPU=test-device driver=test-version");
  for (unsigned i = 0; i < 10000; ++i) LogEvent("pacer", "Pacer timeout frame %u\ncontinuation", i);
  auto events = Read(root / "feverscaler-events.log");
  CHECK(events.find("Pacer timeout frame 0 continuation") != std::string::npos);
  CHECK(events.find("FG=1 x5") != std::string::npos);
  CHECK(events.size() < 2048); // A warning storm must not create per-frame disk traffic.
  LogTestClose();
  events = Read(root / "feverscaler-events.log");
  CHECK(events.find("9999 additional occurrences") != std::string::npos);
  CHECK(events.find("frame 9999 continuation") != std::string::npos);
  LogTestInit(directory);
  CHECK(Read(root / "feverscaler-events.log.previous") == events); // restart preserves evidence
  LogEnvironment("GPU=test-device driver=test-version");
  std::string payload(1600, 'x');
  std::vector<std::thread> writers;
  for (unsigned t = 0; t < 4; ++t) writers.emplace_back([&, t] {
    for (unsigned i = 0; i < 400; ++i) {
      Log("thread=%u record=%u %s", t, i, payload.c_str());
      char key[64]; snprintf(key, sizeof(key), "different-error-%u-%u", t, i);
      LogEvent(key, "error %s", payload.c_str());
    }
  });
  for (auto& w : writers) w.join();
  LogTestClose();
  for (const auto& file : fs::directory_iterator(root)) {
    const auto name = file.path().filename().string();
    CHECK(file.file_size() <= (name.find("events") != std::string::npos ? kEventLogBytes : kLogBytes));
    auto data = Read(file.path());
    CHECK(data.find("GPU=test-device") != std::string::npos);
    CHECK(!data.empty() && data.back() == '\n');
  }
  // A render call that never returns must be recorded by the independent sampler.
  LogTestInit(directory);
  {
    DiagnosticScope call(DiagnosticStage::Present);
    DiagnosticsTestPoll(GetTickCount64() + 2100);
    auto data = Read(root / "feverscaler-events.log");
    CHECK(data.find("vkQueuePresentKHR has not returned") != std::string::npos);
    CHECK(data.find("FG=1 x5") != std::string::npos);
  }
  LogTestClose();
  // A call stalled on another thread is recorded with that thread's stack, walked past the blocking wait.
  LogTestInit(directory);
  {
    HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr), release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread stalled([&] {
      DiagnosticScope call(DiagnosticStage::DlssEvaluate);
      SetEvent(entered);
      WaitForSingleObject(release, INFINITE);
    });
    WaitForSingleObject(entered, INFINITE);
    Sleep(100);
    DiagnosticsTestPoll(GetTickCount64() + 2100);
    SetEvent(release);
    stalled.join();
    CloseHandle(entered); CloseHandle(release);
    auto data = Read(root / "feverscaler-events.log");
    CHECK(data.find("DLSS evaluate has not returned") != std::string::npos);
    CHECK(data.find("; stack: ntdll.dll+0x") != std::string::npos);
    CHECK(data.find(" feverscaler_logging_test.exe+0x") != std::string::npos);
  }
  LogTestClose();
  // System exceptions are logged with their stack, C++ exceptions are not, and modules from outside
  // Windows are listed and logged as they load.
  LogTestInit(directory);
  DiagnosticsInit();
  RaiseHandledAccessViolation();
  try { throw 1; } catch (int) {}
  const auto module = root / "feverscaler-test-module.dll";
  wchar_t system[MAX_PATH]{}; CHECK(GetSystemDirectoryW(system, MAX_PATH));
  CHECK(CopyFileW((fs::path(system) / "version.dll").c_str(), module.c_str(), FALSE));
  HMODULE loaded = LoadLibraryW(module.c_str()); CHECK(loaded); FreeLibrary(loaded);
  loaded = LoadLibraryW(L"version.dll"); CHECK(loaded); FreeLibrary(loaded);
  {
    auto events = Read(root / "feverscaler-events.log");
    CHECK(events.find("exception 0xC0000005 (write to 0x0) on thread") != std::string::npos);
    CHECK(events.find("; stack: feverscaler_logging_test.exe+0x") != std::string::npos);
    CHECK(events.find("0xE06D7363") == std::string::npos);
    auto log = Read(root / "feverscaler.log");
    CHECK(log.find("modules from outside Windows: feverscaler_logging_test.exe") != std::string::npos);
    CHECK(log.find("module loaded: feverscaler-test-module.dll") != std::string::npos);
    CHECK(log.find("module loaded: version.dll") == std::string::npos);
    char description[192]; DiagnosticsSystem(description, sizeof(description));
    const std::string text = description;
    CHECK(text.rfind("Windows=10.0.", 0) == 0 && text.find(" CPU=") != std::string::npos && text.find(" RAM=") != std::string::npos);
  }
  LogTestClose();
  // An idle game / completed calls do not constitute a stall.
  LogTestInit(directory);
  DiagnosticsTestPoll(GetTickCount64() + 120000);
  CHECK(Read(root / "feverscaler-events.log").find("has not returned") == std::string::npos);
  // Exercise the real independent worker, including persistence before a blocked caller returns.
  DiagnosticsStart();
  {
    DiagnosticScope call(DiagnosticStage::Prepass);
    bool captured = false;
    for (unsigned i = 0; i < 40; ++i) {
      Sleep(100);
      if (Read(root / "feverscaler-events.log").find("prepass submit has not returned") != std::string::npos) {
        captured = true; break;
      }
    }
    CHECK(captured);
  }
  Log("preserved evidence"); LogTestClose();
  // A locked log must not be truncated or grow past its bound when rotation fails.
  const auto original = Read(root / "feverscaler.log");
  HANDLE locked = CreateFileW((root / "feverscaler.log").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  CHECK(locked != INVALID_HANDLE_VALUE);
  LogTestInit(directory); Log("must not replace locked evidence"); LogTestClose();
  CHECK(Read(root / "feverscaler.log") == original);
  CloseHandle(locked);
  // This unique test-owned directory was created above and contains only these bounded test logs.
  CHECK(fs::equivalent(root.parent_path(), fs::path(temp)));
  fs::remove_all(root);
  std::puts("bounded diagnostics: passed");
}
