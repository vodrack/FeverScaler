#pragma once
#include <vulkan/vulkan.h>

#include <cstdint>

namespace feverscaler {
// Plugin copies of the game's descriptor sets, bound in place of the game's own.
// Texture mip bias while DLSS upscales: TF3's samplers have a fixed LOD bias, so at a reduced render
// resolution the GPU picks the mips for the render size and DLSS cannot bring the detail back. While
// a bias is set, the world pass binds copies in which the samplers of the mip-mapped textures are
// twins with the extra bias.
// DLSS output: the UI pass binds copies in which the scene image it stretches onto the screen is the
// DLSS output.
// The game's own sets and samplers never change. A copy lives until the game writes or releases its
// set, after which no command buffer that could still be submitted can use either of them.
PFN_vkVoidFunction MipBiasWrapper(const char* name);  // sampler, layout, pool and set lifetime
// After the game's vkUpdateDescriptorSets (the writes as they reached the driver).
void MipBiasOnUpdate(uint32_t writeCount, const VkWriteDescriptorSet* writes, uint32_t copyCount,
                     const VkCopyDescriptorSet* copies);
// Recording thread, world pass: the set to bind instead of `set` (itself when nothing applies).
VkDescriptorSet MipBiasSet(VkDescriptorSet set);
// Recording thread, UI pass: the set to bind instead of `set`, with `output` wherever it samples one
// of the `scene` views (itself when it samples none).
VkDescriptorSet SceneSet(VkDescriptorSet set, const VkImageView* scene, uint32_t sceneCount, VkImageView output);
// Present thread: render width / output width of the DLSS image (0 = no bias), and the frame number
// that paces the release of retired copies.
void MipBiasOnPresent(float renderToOutput, uint64_t frame);
}  // namespace feverscaler
