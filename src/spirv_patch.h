#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace feverscaler {
// Hashed alpha testing for a fragment shader that relied on alpha-to-coverage: discards the
// fragment when its output alpha (location 0) is below a hash of its world position (the
// `posAmbient` input) on a quarter-pixel grid. DLSS's sub-pixel jitter moves the samples between
// grid cells every frame, so partial alpha averages out to partial coverage instead of a fixed
// dot pattern. Returns the patched module, or an empty vector if the shader does not fit.
std::vector<uint32_t> PatchHashedAlpha(const uint32_t* code, size_t sizeBytes);

// Screen-space reflections (ssr_apply.fs), while DLSS runs (the view is jittered):
// - the ray's pixel stride grows with the render height (x1 at 480 lines), so reflections reach as
//   far up the screen at every DLSS preset instead of being cut off at high render resolutions
//   (the game refines every hit to single pixels, so precision stays);
// - no checkerboard ray offset ((int(x) + int(y)) & 1), which DLSS would keep as a dot pattern;
//   instead the pixels of each 2x2 block step at 0, 1/4, 1/2, 3/4 of the stride (ordered dither),
//   rotated every frame by the DLSS jitter, which DLSS averages like the hashed alpha on foliage.
// Without jitter the shader behaves as before.
std::vector<uint32_t> PatchSsr(const uint32_t* code, size_t sizeBytes);
}  // namespace feverscaler
