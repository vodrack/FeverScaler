#pragma once
#include <vulkan/vulkan.h>

#include <cstdint>

#include "tracker.h"

namespace feverscaler {
struct GpuImage {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory mem = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;  // depth images: depth-aspect view
  VkFormat format = VK_FORMAT_UNDEFINED;
  uint32_t w = 0, h = 0;
  VkImageUsageFlags usage = 0;
  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

// Per frame-in-flight resources. Depth, motion vectors and HUD-less colour are tagged for DLSS-G
// with eValidUntilPresent, so each frame gets its own set and nothing is overwritten while
// DLSS-G may still read it.
constexpr uint32_t kSlots = 3;
struct GpuSlot {
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  bool pending = false;
  VkBuffer upload = VK_NULL_HANDLE;  // matrices for the replay pass (host visible)
  VkDeviceMemory uploadMem = VK_NULL_HANDLE;
  uint8_t* uploadPtr = nullptr;
  uint32_t uploadCapacity = 0;  // in mat4
  uint32_t uploadUsed = 0;
  VkBuffer ubo = VK_NULL_HANDLE;
  VkDeviceMemory uboMem = VK_NULL_HANDLE;
  uint8_t* uboPtr = nullptr;
  VkDescriptorSet set = VK_NULL_HANDLE;
  bool setDirty = true;
  GpuImage depth, mv, hudless;
  VkFramebuffer mvFb = VK_NULL_HANDLE;
};

struct FrameUbo {
  float currVP[16];          // unjittered
  float prevVP[16];          // unjittered
  float clipToPrevClip[16];  // jittered raster clip -> previous raster clip
  float size[4];
  float nearFar[4];
  float currVPJit[16];       // what the game rasterised with (DLSS jitter applied)
  float jitter[4];           // xy: jitter in raster NDC (content offset), 0 without DLSS
};

bool GpuInit(uint32_t queueFamily);
bool GpuReady();
GpuSlot& GpuGetSlot(uint32_t i);
void GpuWaitSlot(GpuSlot& s);
bool GpuEnsureRenderTargets(GpuSlot& s, uint32_t w, uint32_t h, VkFormat depthFormat);
bool GpuEnsureHudless(GpuSlot& s, uint32_t w, uint32_t h, VkFormat format);
// DLSS Super Resolution output (display size). One image: the UI pass that samples it is ordered
// before the next frame's DLSS write by the barrier in front of that write.
GpuImage* GpuEnsureSrOutput(uint32_t w, uint32_t h);
GpuImage* GpuSrOutput();
void GpuUpdateDescriptors(GpuSlot& s);
void GpuCollectGarbage(uint64_t frame);
VkPipelineLayout GpuPipelineLayout();
VkPipeline GpuCameraPipeline();
VkRenderPass GpuMvRenderPass();
VkPipeline GpuReplayPipeline(const PipeInfo& p);
}  // namespace feverscaler
