#pragma once
#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace feverscaler {
// What a graphics pipeline is, derived from its SPIR-V names and vertex input state.
struct PipeInfo {
  enum Kind : uint8_t { Other = 0, Mesh, Skinned };
  struct Attr {
    uint32_t binding = ~0u, offset = 0, stride = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    bool valid() const { return binding != ~0u; }
  };
  Kind kind = Other;
  bool mainPass = false;   // created for the main scene render pass
  bool usesView = false;   // reads u_view (set 0, binding 10)
  bool isCompose = false;  // the tonemap/compose pass (u_blur)
  VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  bool primitiveRestart = false;
  Attr pos, infl;                         // attrPosition, attrInfluence (skinned)
  uint32_t modelBinding = ~0u, modelStride = 0, modelOffs[4] = {};  // instAttrModel columns
  // Copy of an alpha-to-coverage pipeline without it, bound instead while DLSS upscales: without
  // MSAA alpha-to-coverage is a fixed dot pattern that DLSS keeps as detail.
  VkPipeline noA2C = VK_NULL_HANDLE;
  VkPipeline srSsr = VK_NULL_HANDLE;
};

// A main-pass draw of a mesh or skinned pipeline, recorded for motion-vector replay.
struct DrawRec {
  const PipeInfo* pipe = nullptr;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkBuffer posBuf = VK_NULL_HANDLE, inflBuf = VK_NULL_HANDLE, modelBuf = VK_NULL_HANDLE, idxBuf = VK_NULL_HANDLE;
  VkDeviceSize posOff = 0, inflOff = 0, modelOff = 0, idxOff = 0;
  VkIndexType idxType = VK_INDEX_TYPE_UINT16;
  bool indexed = false;
  uint32_t count = 0, instanceCount = 0, first = 0, firstInstance = 0;
  int32_t vertexOffset = 0;
  VkDescriptorSet set0 = VK_NULL_HANDLE, set3 = VK_NULL_HANDLE;
  uint32_t dyn3 = 0;
};

// Per command buffer recording state (external synchronisation per Vulkan rules).
struct CbState {
  VkCommandBuffer cb = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  const PipeInfo* pipe = nullptr;
  VkBuffer vb[16] = {};
  VkDeviceSize vbOff[16] = {};
  VkBuffer ib = VK_NULL_HANDLE;
  VkDeviceSize ibOff = 0;
  VkIndexType ibType = VK_INDEX_TYPE_UINT16;
  VkDescriptorSet sets[8] = {};
  uint32_t dyn3 = 0;
  bool inMainPass = false, afterCompose = false;
  bool worldView = false;  // the latest 3D pass in this recording was the world, not a preview
  std::vector<DrawRec> draws;
  // main pass seen in this recording
  bool hasMainPass = false;
  VkImage mainDepth = VK_NULL_HANDLE;
  VkFormat mainDepthFormat = VK_FORMAT_UNDEFINED;
  VkSampleCountFlagBits mainSamples = VkSampleCountFlagBits(0);  // unknown is not copyable
  uint32_t mainW = 0, mainH = 0;
  // HUD-less source: last colour write at swapchain size before the UI pass
  VkImage candImage = VK_NULL_HANDLE;
  VkImageLayout candLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  uint32_t candW = 0, candH = 0;
  bool uiPassSeen = false;
  int hudlessSlot = -1;  // frame slot whose HUD-less image the UI pass copy wrote
  // Set 0 of the first main-pass draw that reads u_view: the camera block (jittered for DLSS).
  VkDescriptorSet viewSet = VK_NULL_HANDLE;
  // Set 0 of every other draw that reads u_view (post-processing such as the screen-space
  // reflections, other 3D passes): jittered as well when it holds the main camera.
  VkDescriptorSet otherViewSets[16] = {};
  uint32_t otherViewCount = 0;
  // DLSS input: the tonemapped scene copied at render size after compose (what FSR1 reads).
  VkImage srIn = VK_NULL_HANDLE;
  VkImageLayout srInLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  uint32_t srInW = 0, srInH = 0;

  void Reset();
};
CbState* GetCbState(VkCommandBuffer cb);

// ---- thread-safe object queries -------------------------------------------------------------
// Copies bytes of a buffer through the application's persistent mapping (false if unmapped).
bool ReadBufferBytes(VkBuffer buf, VkDeviceSize offset, void* dst, size_t size);
struct UboBinding {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceSize offset = 0, range = 0;
  bool dynamic = false;
};
bool GetUboBinding(VkDescriptorSet set, uint32_t binding, UboBinding* out);
bool GetImageInfo(VkImage img, VkFormat* fmt, uint32_t* w, uint32_t* h);
// Writes through the application's persistent mapping (false if unmapped).
bool WriteBufferBytes(VkBuffer buf, VkDeviceSize offset, const void* src, size_t size);
struct ImageDesc {
  VkFormat format = VK_FORMAT_UNDEFINED;
  uint32_t w = 0, h = 0;
  VkImageUsageFlags usage = 0;
  VkSampleCountFlagBits samples = VkSampleCountFlagBits(0);
  VkImageView view = VK_NULL_HANDLE;  // a view the game created of it
};
bool GetImageDesc(VkImage img, ImageDesc* out);
// A mip-mapped image the game only samples (an asset texture, not a render target or storage image).
bool IsMipmappedTexture(VkImageView view);
// DLSS output hand-off: descriptor writes that sample `from` (the image the UI pass stretches onto
// the screen) get `to` instead. Pass nulls to stop.
void SetSceneRedirect(VkImage from, VkImageView to);
void SetSwapchainImages(const VkImage* imgs, uint32_t n);
bool IsSwapchainImage(VkImage img);
void SetSwapchainInfo(VkExtent2D extent, VkFormat format);
VkExtent2D SwapchainExtent();
VkFormat SwapchainFormat();
// When a world-view 3D pass last began (GetTickCount64), 0 if never.
uint64_t LastWorldPassTick();
// Most recently recorded world depth, including passes recorded before their submit.
VkImage WorldDepthImage();
}  // namespace feverscaler
