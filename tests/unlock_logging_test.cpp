// Exercise the patched dependency's real recorder, including live counters and disk quota.
#include "temporal_interval_trace.cpp"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <cstring>
#include <cstdlib>
namespace fs = std::filesystem;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #c); std::exit(1); } } while (0)
struct Parameters : NVSDK_NGX_Parameter {
#define OTHER(T) void Set(const char*, T) override {} \
  NVSDK_NGX_Result Get(const char*, T*) const override { return static_cast<NVSDK_NGX_Result>(0xbad00010); }
  OTHER(unsigned long long)
  OTHER(float)
  OTHER(double)
  OTHER(unsigned int)
  OTHER(ID3D11Resource*)
  OTHER(ID3D12Resource*)
  OTHER(void*)
#undef OTHER
  void Set(const char*, int) override {}
  NVSDK_NGX_Result Get(const char* key, int* value) const override {
    if (!strcmp(key, "DLSSG.MultiFrameCount")) *value = 4;
    else if (!strcmp(key, "DLSSG.MultiFrameIndex")) *value = 1;
    else if (!strcmp(key, "DLSSG.MultiFrameCountMax")) *value = 5;
    else return static_cast<NVSDK_NGX_Result>(0xbad00010);
    return NVSDK_NGX_Result_Success;
  }
  void Reset() override {}
};
int main(int argc, char**) {
  const bool trace = argc > 1;
  SetEnvironmentVariableW(L"FEVERSCALER_UNLOCK_TRACE", trace ? L"1" : nullptr);
  wchar_t temp[MAX_PATH]{}; CHECK(GetTempPathW(MAX_PATH, temp));
  fs::path root = fs::path(temp) / (L"FeverScaler-unlock-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
  CHECK(fs::create_directory(root));
  temporal_interval_trace::Initialize(root.c_str(), GetCurrentProcessId());
  temporal_interval_trace::SetEnabled(true);
  Parameters parameters;
  uint64_t handleStorage = 1; // opaque identity only; the recorder never dereferences a handle
  auto* handle = reinterpret_cast<const NVSDK_NGX_Handle*>(&handleStorage);
  for (unsigned i = 0; i < 50000; ++i) {
    CHECK(temporal_interval_trace::RecordIfValidTemporalSample(handle, &parameters, true));
    if (i % 100 == 0) temporal_interval_trace::Flush();
  }
  temporal_interval_trace::Flush();
  auto snapshot = temporal_interval_trace::ReadSnapshot();
  CHECK(snapshot.validSamples == 50000 && snapshot.invalidSamples == 0);
  CHECK(snapshot.lastCount == 4 && snapshot.lastIndex == 1);
  fs::path output = temporal_interval_trace::FilePath();
  if (trace) {
    CHECK(fs::exists(output) && fs::file_size(output) <= 1024 * 1024);
    std::ifstream file(output); std::string data{std::istreambuf_iterator<char>(file), {}};
    CHECK(data.find("trace size limit reached") != std::string::npos);
    CHECK(!snapshot.logReady);
  } else {
    CHECK(!fs::exists(output));
    CHECK(fs::is_empty(root));
  }
  CHECK(fs::equivalent(root.parent_path(), fs::path(temp)));
  fs::remove_all(root);
  std::puts(trace ? "unlock trace quota: passed" : "unlock counters without CSV: passed");
}
