#pragma once
#include <cstdint>
namespace feverscaler {
// Called first, from DllMain: logs system exceptions and the modules loaded from outside Windows.
void DiagnosticsInit();
// Start outside DllMain. The sampler suspends a stalled thread only to copy its stack and never
// calls Vulkan, Streamline, or game code.
void DiagnosticsStart();
// Windows version, CPU and RAM.
void DiagnosticsSystem(char* buffer, unsigned size);
struct DiagnosticContext {
  uint32_t fgOn = 0, multiplier = 1;
  uint32_t srOn = 0, srMode = 0, srPreset = 0, sceneValid = 0;
  uint32_t renderW = 0, renderH = 0, outputW = 0, outputH = 0;
};
void DiagnosticsContext(const DiagnosticContext& context);
void DiagnosticsDescribe(char* buffer, unsigned size);
enum class DiagnosticStage : uint32_t {
  Present, Submit, Prepass, FenceWait, DlssEvaluate, FgOptions, ReflexSleep, Count
};
class DiagnosticScope {
 public:
  explicit DiagnosticScope(DiagnosticStage stage);
  ~DiagnosticScope();
  DiagnosticScope(const DiagnosticScope&) = delete;
  DiagnosticScope& operator=(const DiagnosticScope&) = delete;
 private:
  int slot_ = -1;
  uint64_t started_ = 0;
  DiagnosticStage stage_;
};
#ifdef FEVERSCALER_TESTS
void DiagnosticsTestPoll(uint64_t now);
#endif
}  // namespace feverscaler
