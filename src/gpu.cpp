#include "gpu.h"

#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "hooks.h"
#include "log.h"
#include "diagnostics.h"

namespace feverscaler {
static const uint32_t kCameraMvSpv[] =
#include "camera_mv.comp.spv.h"
    ;
static const uint32_t kReplayMeshSpv[] =
#include "replay_mesh.vert.spv.h"
    ;
static const uint32_t kReplaySkinnedSpv[] =
#include "replay_skinned.vert.spv.h"
    ;
static const uint32_t kReplayFragSpv[] =
#include "replay.frag.spv.h"
    ;

static bool g_ready = false;
static uint32_t g_family = 0;
static VkCommandPool g_pool{};
static VkDescriptorSetLayout g_setLayout{};
static VkPipelineLayout g_layout{};
static VkDescriptorPool g_descPool{};
static VkSampler g_sampler{};
static VkPipeline g_cameraPipe{};
static VkRenderPass g_mvPass{};
static VkShaderModule g_vsMesh{}, g_vsSkinned{}, g_fs{};
static VkPhysicalDeviceMemoryProperties g_memProps{};
static GpuSlot g_slots[kSlots];
static std::mutex g_pipeMx;
static std::unordered_map<uint64_t, VkPipeline> g_replayPipes;

struct Garbage {
  uint64_t frame;
  GpuImage img;
  VkFramebuffer fb;
};
static std::vector<Garbage> g_garbage;
static uint64_t g_frameForGarbage = 0;

static uint32_t MemType(uint32_t bits, VkMemoryPropertyFlags want) {
  for (uint32_t i = 0; i < g_memProps.memoryTypeCount; ++i)
    if ((bits & (1u << i)) && (g_memProps.memoryTypes[i].propertyFlags & want) == want) return i;
  return UINT32_MAX;
}

static bool MakeBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer* buf, VkDeviceMemory* mem, uint8_t** ptr) {
  VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bi.size = size;
  bi.usage = usage;
  bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vk.vkCreateBuffer(gDevice, &bi, nullptr, buf) != VK_SUCCESS) return false;
  VkMemoryRequirements mr;
  vk.vkGetBufferMemoryRequirements(gDevice, *buf, &mr);
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = mr.size;
  ai.memoryTypeIndex = MemType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (ai.memoryTypeIndex == UINT32_MAX || vk.vkAllocateMemory(gDevice, &ai, nullptr, mem) != VK_SUCCESS) return false;
  if (vk.vkBindBufferMemory(gDevice, *buf, *mem, 0) != VK_SUCCESS) return false;
  return vk.vkMapMemory(gDevice, *mem, 0, VK_WHOLE_SIZE, 0, (void**)ptr) == VK_SUCCESS;
}

static bool MakeImage(GpuImage& im, uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage, VkImageAspectFlags aspect) {
  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ci.imageType = VK_IMAGE_TYPE_2D;
  ci.format = fmt;
  ci.extent = {w, h, 1};
  ci.mipLevels = 1;
  ci.arrayLayers = 1;
  ci.samples = VK_SAMPLE_COUNT_1_BIT;
  ci.tiling = VK_IMAGE_TILING_OPTIMAL;
  ci.usage = usage;
  ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (vk.vkCreateImage(gDevice, &ci, nullptr, &im.image) != VK_SUCCESS) return false;
  VkMemoryRequirements mr;
  vk.vkGetImageMemoryRequirements(gDevice, im.image, &mr);
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = mr.size;
  ai.memoryTypeIndex = MemType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (ai.memoryTypeIndex == UINT32_MAX || vk.vkAllocateMemory(gDevice, &ai, nullptr, &im.mem) != VK_SUCCESS) return false;
  if (vk.vkBindImageMemory(gDevice, im.image, im.mem, 0) != VK_SUCCESS) return false;
  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vi.image = im.image;
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vi.format = fmt;
  vi.subresourceRange = {aspect, 0, 1, 0, 1};
  if (vk.vkCreateImageView(gDevice, &vi, nullptr, &im.view) != VK_SUCCESS) return false;
  im.format = fmt;
  im.w = w;
  im.h = h;
  im.usage = usage;
  im.layout = VK_IMAGE_LAYOUT_UNDEFINED;
  return true;
}

static void Retire(GpuImage& im, VkFramebuffer fb = VK_NULL_HANDLE) {
  if (im.image || fb) g_garbage.push_back({g_frameForGarbage + 8, im, fb});
  im = GpuImage{};
}

void GpuCollectGarbage(uint64_t frame) {
  g_frameForGarbage = frame;
  for (size_t i = 0; i < g_garbage.size();) {
    if (g_garbage[i].frame > frame) {
      ++i;
      continue;
    }
    Garbage& g = g_garbage[i];
    if (g.fb) vk.vkDestroyFramebuffer(gDevice, g.fb, nullptr);
    if (g.img.view) vk.vkDestroyImageView(gDevice, g.img.view, nullptr);
    if (g.img.image) vk.vkDestroyImage(gDevice, g.img.image, nullptr);
    if (g.img.mem) vk.vkFreeMemory(gDevice, g.img.mem, nullptr);
    g_garbage[i] = g_garbage.back();
    g_garbage.pop_back();
  }
}

static VkShaderModule MakeModule(const uint32_t* code, size_t bytes) {
  VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  ci.codeSize = bytes;
  ci.pCode = code;
  VkShaderModule m{};
  vk.vkCreateShaderModule(gDevice, &ci, nullptr, &m);
  return m;
}

bool GpuInit(uint32_t family) {
  if (g_ready) return true;
  static bool tried = false;
  if (tried) return false;
  tried = true;
  g_family = family;
  vk.vkGetPhysicalDeviceMemoryProperties(gPhysicalDevice, &g_memProps);

  VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pi.queueFamilyIndex = family;
  if (vk.vkCreateCommandPool(gDevice, &pi, nullptr, &g_pool) != VK_SUCCESS) return false;

  VkDescriptorSetLayoutBinding b[4] = {
      {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr},
      {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
  };
  VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  li.bindingCount = 4;
  li.pBindings = b;
  if (vk.vkCreateDescriptorSetLayout(gDevice, &li, nullptr, &g_setLayout) != VK_SUCCESS) return false;
  VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT, 0, 16};
  VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pli.setLayoutCount = 1;
  pli.pSetLayouts = &g_setLayout;
  pli.pushConstantRangeCount = 1;
  pli.pPushConstantRanges = &pcr;
  if (vk.vkCreatePipelineLayout(gDevice, &pli, nullptr, &g_layout) != VK_SUCCESS) return false;

  VkDescriptorPoolSize ps[4] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kSlots},
                                {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kSlots},
                                {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kSlots},
                                {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kSlots}};
  VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dpi.maxSets = kSlots;
  dpi.poolSizeCount = 4;
  dpi.pPoolSizes = ps;
  if (vk.vkCreateDescriptorPool(gDevice, &dpi, nullptr, &g_descPool) != VK_SUCCESS) return false;

  VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  si.magFilter = si.minFilter = VK_FILTER_NEAREST;
  si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  si.maxLod = 0;
  if (vk.vkCreateSampler(gDevice, &si, nullptr, &g_sampler) != VK_SUCCESS) return false;

  // Camera-motion compute pipeline.
  VkShaderModule cs = MakeModule(kCameraMvSpv, sizeof(kCameraMvSpv));
  VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, cs, "main", nullptr};
  cpi.layout = g_layout;
  if (vk.vkCreateComputePipelines(gDevice, VK_NULL_HANDLE, 1, &cpi, nullptr, &g_cameraPipe) != VK_SUCCESS) return false;
  vk.vkDestroyShaderModule(gDevice, cs, nullptr);

  // Motion-vector render pass for the replay: the camera pass already filled the image.
  VkAttachmentDescription ad{};
  ad.format = VK_FORMAT_R16G16_SFLOAT;
  ad.samples = VK_SAMPLE_COUNT_1_BIT;
  ad.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  ad.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  ad.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  ad.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  ad.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  ad.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  VkAttachmentReference ar{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sd{};
  sd.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sd.colorAttachmentCount = 1;
  sd.pColorAttachments = &ar;
  VkSubpassDependency deps[2] = {};
  deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
  deps[0].dstSubpass = 0;
  deps[0].srcStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
  deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  deps[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
  deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
  deps[1].srcSubpass = 0;
  deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
  deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  deps[1].dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
  deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
  VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  rpi.attachmentCount = 1;
  rpi.pAttachments = &ad;
  rpi.subpassCount = 1;
  rpi.pSubpasses = &sd;
  rpi.dependencyCount = 2;
  rpi.pDependencies = deps;
  if (vk.vkCreateRenderPass(gDevice, &rpi, nullptr, &g_mvPass) != VK_SUCCESS) return false;

  g_vsMesh = MakeModule(kReplayMeshSpv, sizeof(kReplayMeshSpv));
  g_vsSkinned = MakeModule(kReplaySkinnedSpv, sizeof(kReplaySkinnedSpv));
  g_fs = MakeModule(kReplayFragSpv, sizeof(kReplayFragSpv));

  for (uint32_t i = 0; i < kSlots; ++i) {
    GpuSlot& s = g_slots[i];
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = g_pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    if (vk.vkAllocateCommandBuffers(gDevice, &cai, &s.cmd) != VK_SUCCESS) return false;
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (vk.vkCreateFence(gDevice, &fi, nullptr, &s.fence) != VK_SUCCESS) return false;
    const uint32_t mats = 1u << 18;  // 16 MB of mat4
    if (!MakeBuffer((VkDeviceSize)mats * 64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &s.upload, &s.uploadMem, &s.uploadPtr))
      return false;
    s.uploadCapacity = mats;
    if (!MakeBuffer((sizeof(FrameUbo) + 255) & ~(VkDeviceSize)255, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &s.ubo, &s.uboMem, &s.uboPtr)) return false;
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = g_descPool;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &g_setLayout;
    if (vk.vkAllocateDescriptorSets(gDevice, &dai, &s.set) != VK_SUCCESS) return false;
  }
  g_ready = true;
  Log("GPU resources ready (queue family %u)", family);
  return true;
}

bool GpuReady() { return g_ready; }
GpuSlot& GpuGetSlot(uint32_t i) { return g_slots[i % kSlots]; }

void GpuWaitSlot(GpuSlot& s) {
  if (!s.pending) return;
  DiagnosticScope scope(DiagnosticStage::FenceWait);
  VkResult result = vk.vkWaitForFences(gDevice, 1, &s.fence, VK_TRUE, 2000000000ull);
  if (result != VK_SUCCESS) LogEvent("GPU-fence", "pre-pass fence wait result=%d", (int)result);
  s.pending = false;
}

bool GpuEnsureRenderTargets(GpuSlot& s, uint32_t w, uint32_t h, VkFormat depthFormat) {
  if (s.depth.image && s.depth.w == w && s.depth.h == h && s.depth.format == depthFormat && s.mv.image) return true;
  Retire(s.depth);
  Retire(s.mv, s.mvFb);
  s.mvFb = VK_NULL_HANDLE;
  if (!MakeImage(s.depth, w, h, depthFormat,
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                     VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                 VK_IMAGE_ASPECT_DEPTH_BIT))
    return false;
  if (!MakeImage(s.mv, w, h, VK_FORMAT_R16G16_SFLOAT,
                 VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                 VK_IMAGE_ASPECT_COLOR_BIT))
    return false;
  VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  fi.renderPass = g_mvPass;
  fi.attachmentCount = 1;
  fi.pAttachments = &s.mv.view;
  fi.width = w;
  fi.height = h;
  fi.layers = 1;
  if (vk.vkCreateFramebuffer(gDevice, &fi, nullptr, &s.mvFb) != VK_SUCCESS) return false;
  s.setDirty = true;
  Log("render targets %ux%u (depth fmt %d)", w, h, (int)depthFormat);
  return true;
}

bool GpuEnsureHudless(GpuSlot& s, uint32_t w, uint32_t h, VkFormat format) {
  if (s.hudless.image && s.hudless.w == w && s.hudless.h == h && s.hudless.format == format) return true;
  Retire(s.hudless);
  if (!MakeImage(s.hudless, w, h, format,
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                 VK_IMAGE_ASPECT_COLOR_BIT))
    return false;
  Log("HUD-less image %ux%u fmt %d", w, h, (int)format);
  return true;
}

static GpuImage g_srOut;
GpuImage* GpuSrOutput() { return g_srOut.image ? &g_srOut : nullptr; }
GpuImage* GpuEnsureSrOutput(uint32_t w, uint32_t h) {
  if (g_srOut.image && g_srOut.w == w && g_srOut.h == h) return &g_srOut;
  Retire(g_srOut);
  if (!MakeImage(g_srOut, w, h, VK_FORMAT_R8G8B8A8_UNORM,
                 VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                 VK_IMAGE_ASPECT_COLOR_BIT)) {
    Retire(g_srOut);
    return nullptr;
  }
  Log("DLSS output image %ux%u", w, h);
  return &g_srOut;
}

void GpuUpdateDescriptors(GpuSlot& s) {
  if (!s.setDirty) return;
  VkDescriptorBufferInfo up{s.upload, 0, VK_WHOLE_SIZE};
  VkDescriptorImageInfo depth{g_sampler, s.depth.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
  VkDescriptorBufferInfo ubo{s.ubo, 0, sizeof(FrameUbo)};
  VkDescriptorImageInfo mv{VK_NULL_HANDLE, s.mv.view, VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet w[4] = {};
  for (int i = 0; i < 4; ++i) {
    w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[i].dstSet = s.set;
    w[i].dstBinding = (uint32_t)i;
    w[i].descriptorCount = 1;
  }
  w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  w[0].pBufferInfo = &up;
  w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  w[1].pImageInfo = &depth;
  w[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  w[2].pBufferInfo = &ubo;
  w[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  w[3].pImageInfo = &mv;
  vk.vkUpdateDescriptorSets(gDevice, 4, w, 0, nullptr);
  s.setDirty = false;
}

VkPipelineLayout GpuPipelineLayout() { return g_layout; }
VkPipeline GpuCameraPipeline() { return g_cameraPipe; }
VkRenderPass GpuMvRenderPass() { return g_mvPass; }

VkPipeline GpuReplayPipeline(const PipeInfo& p) {
  bool skinned = p.kind == PipeInfo::Skinned;
  uint64_t key = (uint64_t)skinned | ((uint64_t)p.pos.stride << 1) | ((uint64_t)p.pos.offset << 9) |
                 ((uint64_t)p.pos.format << 17) | ((uint64_t)p.topology << 25) | ((uint64_t)p.primitiveRestart << 29);
  if (skinned) key ^= ((uint64_t)p.infl.stride << 32) | ((uint64_t)p.infl.offset << 40) | ((uint64_t)p.infl.format << 48);
  std::lock_guard<std::mutex> lk(g_pipeMx);
  auto it = g_replayPipes.find(key);
  if (it != g_replayPipes.end()) return it->second;

  VkPipelineShaderStageCreateInfo st[2] = {
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT,
       skinned ? g_vsSkinned : g_vsMesh, "main", nullptr},
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, g_fs, "main", nullptr}};
  VkVertexInputBindingDescription vb[2] = {{0, p.pos.stride, VK_VERTEX_INPUT_RATE_VERTEX},
                                           {1, p.infl.stride, VK_VERTEX_INPUT_RATE_VERTEX}};
  VkVertexInputAttributeDescription va[2] = {{0, 0, p.pos.format, p.pos.offset}, {1, 1, p.infl.format, p.infl.offset}};
  VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vi.vertexBindingDescriptionCount = skinned ? 2 : 1;
  vi.pVertexBindingDescriptions = vb;
  vi.vertexAttributeDescriptionCount = skinned ? 2 : 1;
  vi.pVertexAttributeDescriptions = va;
  VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = p.topology;
  ia.primitiveRestartEnable = p.primitiveRestart;
  VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount = 1;
  vp.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = VK_CULL_MODE_NONE;
  rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rs.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState cba{};
  cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT;
  VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cb.attachmentCount = 1;
  cb.pAttachments = &cba;
  VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  ds.dynamicStateCount = 2;
  ds.pDynamicStates = dyn;
  VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  gi.stageCount = 2;
  gi.pStages = st;
  gi.pVertexInputState = &vi;
  gi.pInputAssemblyState = &ia;
  gi.pViewportState = &vp;
  gi.pRasterizationState = &rs;
  gi.pMultisampleState = &ms;
  gi.pColorBlendState = &cb;
  gi.pDynamicState = &ds;
  gi.layout = g_layout;
  gi.renderPass = g_mvPass;
  VkPipeline pipe = VK_NULL_HANDLE;
  VkResult r = vk.vkCreateGraphicsPipelines(gDevice, VK_NULL_HANDLE, 1, &gi, nullptr, &pipe);
  Log("replay pipeline (%s, pos stride %u off %u fmt %d, topo %d) -> %d", skinned ? "skinned" : "mesh", p.pos.stride,
      p.pos.offset, (int)p.pos.format, (int)p.topology, (int)r);
  g_replayPipes[key] = r == VK_SUCCESS ? pipe : VK_NULL_HANDLE;
  return g_replayPipes[key];
}
}  // namespace feverscaler
