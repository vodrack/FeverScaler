#pragma once
#include <cstdint>
#include <vector>

namespace feverscaler {
// Matrices are column-major world transforms and must remain alive for this call.
// Returns previous-array indices, or -1 when history is missing/ambiguous. Matching
// is independent of draw ordering and uses each previous instance at most once.
void MatchMotionInstances(const std::vector<const float*>& current,
                          const std::vector<const float*>& previous,
                          float maxDistance, std::vector<int32_t>& matches);
}  // namespace feverscaler
