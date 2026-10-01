#pragma once
#include <vulkan/vulkan.h>

#include <cstdint>

namespace feverscaler {
// Texture mip bias while DLSS upscales. TF3's samplers have a fixed LOD bias, so at a reduced render
// resolution the GPU picks the mips for the render size and DLSS cannot bring the detail back. While
// a bias is set, the world pass binds copies of the game's descriptor sets in which the samplers of
// its mip-mapped textures are twins with the extra bias. The game's own sets and samplers never
// change. A copy lives until the game writes or releases its set, after which no command buffer
// that could still be submitted can use either of them.
PFN_vkVoidFunction MipBiasWrapper(const char* name);  // sampler, layout, pool and set lifetime
// After the game's vkUpdateDescriptorSets (the writes as they reached the driver).
void MipBiasOnUpdate(uint32_t writeCount, const VkWriteDescriptorSet* writes, uint32_t copyCount,
                     const VkCopyDescriptorSet* copies);
// Recording thread, world pass: the set to bind instead of `set` (itself when nothing applies).
VkDescriptorSet MipBiasSet(VkDescriptorSet set);
// Present thread: render width / output width of the DLSS image (0 = no bias), and the frame number
// that paces the release of retired copies.
void MipBiasOnPresent(float renderToOutput, uint64_t frame);
}  // namespace feverscaler
