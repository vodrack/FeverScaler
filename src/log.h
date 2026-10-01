#pragma once
#include <atomic>
#include <cstdint>
#include <string>

namespace feverscaler {
void LogInit();
void Log(const char* fmt, ...);
void LogEvent(const char* key, const char* fmt, ...);
void LogFlushRepeats();
void LogEnvironment(const char* fmt, ...);
constexpr size_t kLogBytes = 512 * 1024;
constexpr size_t kEventLogBytes = 256 * 1024;
#ifdef FEVERSCALER_TESTS
void LogTestInit(const std::wstring& directory);
void LogTestClose();
#endif
// Log the first `limit` occurrences of a message identified by a static counter.
#define FEVERSCALER_LOG_N(limit, ...)                                                              \
  do {                                                                                             \
    static std::atomic<unsigned> _feverscaler_n{0};                                                \
    unsigned _n = _feverscaler_n.load(std::memory_order_relaxed);                                  \
    while (_n < (limit) && !_feverscaler_n.compare_exchange_weak(_n, _n + 1)) {}                    \
    if (_n < (limit)) ::feverscaler::Log(__VA_ARGS__);                                               \
  } while (0)
// Directory containing this plugin (with trailing backslash) and the game executable.
const std::wstring& PluginDir();
const std::wstring& GameDir();
}  // namespace feverscaler
