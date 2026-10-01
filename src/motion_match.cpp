#include "motion_match.h"
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_map>

namespace feverscaler {
namespace {
struct Cell {
  int32_t x, y, z;
  bool operator==(const Cell& o) const { return x == o.x && y == o.y && z == o.z; }
};
struct Hash {
  size_t operator()(const Cell& c) const {
    uint64_t h = (uint32_t)c.x;
    h = (h ^ (uint32_t)c.y) * 1099511628211ull;
    return (size_t)((h ^ (uint32_t)c.z) * 1099511628211ull);
  }
};
using Grid = std::unordered_map<Cell, std::vector<uint32_t>, Hash>;
bool Locate(const float* matrix, float size, Cell& cell) {
  if (!matrix) return false;
  for (unsigned i = 0; i < 16; ++i) if (!std::isfinite(matrix[i])) return false;
  int32_t coords[3];
  for (unsigned i = 0; i < 3; ++i) {
    double value = std::floor((double)matrix[12 + i] / size);
    if (value < INT32_MIN + 1.0 || value > INT32_MAX - 1.0) return false;
    coords[i] = (int32_t)value;
  }
  cell = {coords[0], coords[1], coords[2]};
  return true;
}
Grid MakeGrid(const std::vector<const float*>& matrices, float size, const std::vector<uint8_t>* used = nullptr) {
  Grid grid;
  grid.reserve(matrices.size());
  for (uint32_t i = 0; i < matrices.size(); ++i) {
    if (used && (*used)[i]) continue;
    Cell cell{};
    if (Locate(matrices[i], size, cell)) grid[cell].push_back(i);
  }
  return grid;
}
int32_t Nearest(const float* matrix, const std::vector<const float*>& candidates,
                const Grid& grid, const std::vector<uint8_t>& used, float distance) {
  Cell cell{};
  if (!Locate(matrix, 2 * distance, cell)) return -1;
  float best = std::numeric_limits<float>::infinity(), second = best;
  int32_t index = -1;
  for (int x = -1; x <= 1; ++x) for (int y = -1; y <= 1; ++y) for (int z = -1; z <= 1; ++z) {
    auto it = grid.find({cell.x + x, cell.y + y, cell.z + z});
    if (it == grid.end()) continue;
    for (uint32_t i : it->second) {
      if (used[i]) continue;
      const float* p = candidates[i];
      float dx = matrix[12] - p[12], dy = matrix[13] - p[13], dz = matrix[14] - p[14];
      float d = dx * dx + dy * dy + dz * dz;
      if (d > 4 * distance * distance) continue;
      if (d < best) { second = best; best = d; index = (int32_t)i; }
      else if (d < second) second = d;
    }
  }
  // A near tie between repeated cars is insufficient evidence of identity. Prefer
  // camera-only motion over inventing a jump to a neighboring object's transform.
  if (best > distance * distance || second <= best * 4.0f + 1e-6f) return -1;
  return index;
}
}  // namespace
void MatchMotionInstances(const std::vector<const float*>& current,
                          const std::vector<const float*>& previous,
                          float maxDistance, std::vector<int32_t>& matches) {
  matches.assign(current.size(), -1);
  if (current.empty() || previous.empty() || !std::isfinite(maxDistance) || maxDistance <= 0) return;
  bool sameOrder = current.size() == previous.size();
  if (sameOrder) for (uint32_t i = 0; i < current.size(); ++i) {
    if (!current[i] || !previous[i] || memcmp(current[i], previous[i], 64)) { sameOrder = false; break; }
    for (unsigned j = 0; j < 16; ++j) if (!std::isfinite(current[i][j])) { sameOrder = false; break; }
    if (!sameOrder) break;
  }
  if (sameOrder) { for (uint32_t i = 0; i < current.size(); ++i) matches[i] = (int32_t)i; return; }
  // A cell twice the match radius also exposes near competitors just outside it.
  Grid previousGrid = MakeGrid(previous, 2 * maxDistance);
  std::vector<uint8_t> usedPrevious(previous.size(), 0), usedCurrent(current.size(), 0);
  // Exact stationary matches first: fast, safe under culling, and they cannot be
  // stolen as history by a different moving instance nearby.
  for (uint32_t i = 0; i < current.size(); ++i) {
    Cell cell{};
    if (!Locate(current[i], 2 * maxDistance, cell)) continue;
    auto it = previousGrid.find(cell);
    if (it == previousGrid.end()) continue;
    for (uint32_t j : it->second) if (!usedPrevious[j] && !memcmp(current[i], previous[j], 64)) {
      matches[i] = (int32_t)j; usedPrevious[j] = usedCurrent[i] = 1; break;
    }
  }
  bool unmatched = false;
  for (auto used : usedCurrent) if (!used) { unmatched = true; break; }
  if (!unmatched) return;
  Grid currentGrid = MakeGrid(current, 2 * maxDistance, &usedCurrent);
  std::vector<int32_t> forward(current.size(), -1), backward(previous.size(), -1);
  for (uint32_t i = 0; i < current.size(); ++i) if (!usedCurrent[i])
    forward[i] = Nearest(current[i], previous, previousGrid, usedPrevious, maxDistance);
  for (uint32_t i = 0; i < previous.size(); ++i) if (!usedPrevious[i])
    backward[i] = Nearest(previous[i], current, currentGrid, usedCurrent, maxDistance);
  // Mutual nearest neighbors are one-to-one and do not depend on traversal order.
  for (uint32_t i = 0; i < current.size(); ++i) if (forward[i] >= 0 && backward[forward[i]] == (int32_t)i)
    matches[i] = forward[i];
}
}  // namespace feverscaler
