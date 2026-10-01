#pragma once
#include <cstdint>

namespace feverscaler {
// This process's use of the GPU's local video memory and the budget Windows gives it (DXGI, which
// sees Vulkan allocations too). Past the budget, memory is paged to system RAM and frames stutter.
struct VramInfo {
  bool valid = false;
  uint64_t budget = 0, usage = 0;
};
VramInfo QueryVram();
}  // namespace feverscaler
