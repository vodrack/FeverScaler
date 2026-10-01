// Object and command-buffer tracking: bookkeeping around the game's own calls. What it changes in
// the game's rendering: while DLSS upscales the world, its binds swap in the SR pipeline copies and
// the mip-biased descriptor set copies (mip_bias.cpp); the HUD-less copy is recorded at the start
// of the UI pass (frame.cpp).
#include "tracker.h"

#include <windows.h>
#include <smmintrin.h>

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>

#include "config.h"
#include "frame.h"
#include "game_scale.h"
#include "hooks.h"
#include "log.h"
#include "mip_bias.h"
#include "spirv.h"
#include "spirv_patch.h"

namespace feverscaler {
// ---- object maps -------------------------------------------------------------------------------
struct ImgInfo {
  VkFormat format;
  uint32_t w, h;
  VkImageUsageFlags usage;
  VkSampleCountFlagBits samples;
  std::vector<VkImageView> views;
  uint32_t mipLevels = 1;
};
struct ViewInfo {
  VkImage image;
};
struct RpInfo {
  uint32_t colorCount = 0;
  bool hasDepth = false;
  bool isMain = false;
  VkFormat depthFormat = VK_FORMAT_UNDEFINED;
  uint32_t depthIndex = ~0u;
  std::vector<uint32_t> colorIndex;
  std::vector<VkImageLayout> finalLayouts;
};
struct FbInfo {
  VkRenderPass rp;
  std::vector<VkImageView> views;
  uint32_t w, h;
};
struct BufInfo {
  VkDeviceSize size = 0;
  VkDeviceMemory mem = VK_NULL_HANDLE;
  VkDeviceSize memOffset = 0;
};
struct MemInfo {
  uint8_t* mapped = nullptr;
  VkDeviceSize mapOffset = 0, mapSize = 0, size = 0;
};
struct DescBindings {
  UboBinding b10, b11;
};

static std::shared_mutex g_mx;  // guards every map below
static std::unordered_map<VkShaderModule, std::shared_ptr<SpirvInfo>> g_modules;
static std::unordered_map<VkPipeline, PipeInfo*> g_pipes;
static std::unordered_map<VkRenderPass, RpInfo> g_rps;
static std::unordered_map<VkFramebuffer, FbInfo> g_fbs;
static std::unordered_map<VkImage, ImgInfo> g_imgs;
static std::unordered_map<VkImageView, ViewInfo> g_views;
static std::unordered_map<VkBuffer, BufInfo> g_bufs;
static std::unordered_map<VkDeviceMemory, MemInfo> g_mems;
static std::unordered_map<VkDescriptorSet, DescBindings> g_sets;
static std::unordered_set<VkImage> g_swapImages;
static VkExtent2D g_swapExtent{};
static VkFormat g_swapFormat = VK_FORMAT_UNDEFINED;
static std::atomic<VkImage> g_worldDepth{VK_NULL_HANDLE};
VkImage WorldDepthImage() { return g_worldDepth.load(); }

void SetSwapchainImages(const VkImage* imgs, uint32_t n) {
  std::unique_lock lk(g_mx);
  for (uint32_t i = 0; i < n; ++i) g_swapImages.insert(imgs[i]);
  FEVERSCALER_LOG_N(4, "swapchain images: %u (first %p)", n, n ? (void*)imgs[0] : nullptr);
}
bool IsSwapchainImage(VkImage img) {
  std::shared_lock lk(g_mx);
  return g_swapImages.count(img) != 0;
}
void SetSwapchainInfo(VkExtent2D e, VkFormat f) {
  std::unique_lock lk(g_mx);
  g_swapExtent = e;
  g_swapFormat = f;
}
VkExtent2D SwapchainExtent() {
  std::shared_lock lk(g_mx);
  return g_swapExtent;
}
VkFormat SwapchainFormat() {
  std::shared_lock lk(g_mx);
  return g_swapFormat;
}

// The game's instance and uniform data live in HOST_VISIBLE|HOST_COHERENT system memory that is
// not CPU-cached (write-combined); ordinary loads from it are slow, streaming loads are not.
static void StreamCopy(void* dst, const void* src, size_t size) {
  const uint8_t* s = (const uint8_t*)src;
  uint8_t* d = (uint8_t*)dst;
  while (((uintptr_t)s & 15) && size) {
    *d++ = *s++;
    --size;
  }
  for (; size >= 64; size -= 64, s += 64, d += 64) {
    __m128i a = _mm_stream_load_si128((__m128i*)(s + 0));
    __m128i b = _mm_stream_load_si128((__m128i*)(s + 16));
    __m128i c = _mm_stream_load_si128((__m128i*)(s + 32));
    __m128i e = _mm_stream_load_si128((__m128i*)(s + 48));
    _mm_storeu_si128((__m128i*)(d + 0), a);
    _mm_storeu_si128((__m128i*)(d + 16), b);
    _mm_storeu_si128((__m128i*)(d + 32), c);
    _mm_storeu_si128((__m128i*)(d + 48), e);
  }
  if (size) memcpy(d, s, size);
}

bool ReadBufferBytes(VkBuffer buf, VkDeviceSize offset, void* dst, size_t size) {
  std::shared_lock lk(g_mx);
  auto b = g_bufs.find(buf);
  if (b == g_bufs.end() || !b->second.mem || offset + size > b->second.size) return false;
  auto m = g_mems.find(b->second.mem);
  if (m == g_mems.end() || !m->second.mapped) return false;
  VkDeviceSize abs = b->second.memOffset + offset;
  const MemInfo& mi = m->second;
  if (abs < mi.mapOffset || abs + size > mi.mapOffset + mi.mapSize) return false;
  StreamCopy(dst, mi.mapped + (abs - mi.mapOffset), size);
  return true;
}

bool GetUboBinding(VkDescriptorSet set, uint32_t binding, UboBinding* out) {
  std::shared_lock lk(g_mx);
  auto it = g_sets.find(set);
  if (it == g_sets.end()) return false;
  const UboBinding& b = binding == 10 ? it->second.b10 : it->second.b11;
  if (!b.buffer) return false;
  *out = b;
  return true;
}

bool WriteBufferBytes(VkBuffer buf, VkDeviceSize offset, const void* src, size_t size) {
  std::shared_lock lk(g_mx);
  auto b = g_bufs.find(buf);
  if (b == g_bufs.end() || !b->second.mem || offset + size > b->second.size) return false;
  auto m = g_mems.find(b->second.mem);
  if (m == g_mems.end() || !m->second.mapped) return false;
  VkDeviceSize abs = b->second.memOffset + offset;
  const MemInfo& mi = m->second;
  if (abs < mi.mapOffset || abs + size > mi.mapOffset + mi.mapSize) return false;
  memcpy(mi.mapped + (abs - mi.mapOffset), src, size);
  return true;
}

bool GetImageDesc(VkImage img, ImageDesc* out) {
  std::shared_lock lk(g_mx);
  auto it = g_imgs.find(img);
  if (it == g_imgs.end()) return false;
  out->format = it->second.format;
  out->w = it->second.w;
  out->h = it->second.h;
  out->usage = it->second.usage;
  out->samples = it->second.samples;
  out->view = it->second.views.empty() ? VK_NULL_HANDLE : it->second.views.front();
  return true;
}

void ShowSceneOutput(CbState* st, VkImageView output) {
  st->sceneViewCount = 0;
  {
    std::shared_lock lk(g_mx);
    auto it = g_imgs.find(st->candImage);
    if (it != g_imgs.end())
      for (VkImageView v : it->second.views)
        if (st->sceneViewCount < std::size(st->sceneViews)) st->sceneViews[st->sceneViewCount++] = v;
  }
  st->sceneOut = st->sceneViewCount ? output : VK_NULL_HANDLE;
  static std::atomic<VkImage> logged{VK_NULL_HANDLE};
  if (logged.exchange(st->candImage) != st->candImage)
    Log("UI pass: scene image %p (%u views) shows the DLSS output", (void*)st->candImage, st->sceneViewCount);
}

bool IsMipmappedTexture(VkImageView view) {
  constexpr VkImageUsageFlags kRendered = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                          VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
  std::shared_lock lk(g_mx);
  auto v = g_views.find(view);
  if (v == g_views.end()) return false;
  auto it = g_imgs.find(v->second.image);
  return it != g_imgs.end() && it->second.mipLevels > 1 && !(it->second.usage & kRendered);
}

bool GetImageInfo(VkImage img, VkFormat* fmt, uint32_t* w, uint32_t* h) {
  std::shared_lock lk(g_mx);
  auto it = g_imgs.find(img);
  if (it == g_imgs.end()) return false;
  if (fmt) *fmt = it->second.format;
  if (w) *w = it->second.w;
  if (h) *h = it->second.h;
  return true;
}

// ---- command buffer state ----------------------------------------------------------------------
void CbState::Reset() {
  VkCommandBuffer keep = cb;
  std::vector<DrawRec> d = std::move(draws);
  d.clear();
  *this = CbState();
  cb = keep;
  draws = std::move(d);
}

static std::mutex g_cbMx;
static std::unordered_map<VkCommandBuffer, CbState*> g_cbs;  // never erased: handles get reused
static thread_local VkCommandBuffer t_lastCb = VK_NULL_HANDLE;
static thread_local CbState* t_lastSt = nullptr;

CbState* GetCbState(VkCommandBuffer cb) {
  if (cb == t_lastCb && t_lastSt) return t_lastSt;
  std::lock_guard<std::mutex> lk(g_cbMx);
  CbState*& st = g_cbs[cb];
  if (!st) {
    st = new CbState();
    st->cb = cb;
  }
  t_lastCb = cb;
  t_lastSt = st;
  return st;
}

// ---- resource wrappers -------------------------------------------------------------------------
static VKAPI_ATTR VkResult VKAPI_CALL w_CreateShaderModule(VkDevice d, const VkShaderModuleCreateInfo* ci,
                                                           const VkAllocationCallbacks* a, VkShaderModule* out) {
  auto info = std::make_shared<SpirvInfo>(ReflectSpirv(ci->pCode, ci->codeSize));
  // Keep the game's original module. The patched variant belongs only to SR pipeline copies.
  if (info->HasBinding("u_ssao") && info->HasBinding("depthBufTex") && info->HasBinding("u_view"))
    info->srCode = PatchSsr(ci->pCode, ci->codeSize);
  VkResult r = vk.vkCreateShaderModule(d, ci, a, out);
  if (r == VK_SUCCESS) {
    if (info->InputLocation("posAmbient") >= 0) info->code.assign(ci->pCode, ci->pCode + ci->codeSize / 4);
    std::unique_lock lk(g_mx);
    g_modules[*out] = std::move(info);
  }
  return r;
}
static VKAPI_ATTR void VKAPI_CALL w_DestroyShaderModule(VkDevice d, VkShaderModule m, const VkAllocationCallbacks* a) {
  {
    std::unique_lock lk(g_mx);
    g_modules.erase(m);
  }
  vk.vkDestroyShaderModule(d, m, a);
}

static void RecordRenderPass(VkRenderPass rp, uint32_t attCount, const VkFormat* formats, const VkImageLayout* finals,
                             uint32_t colorCount, const uint32_t* colorIdx, uint32_t depthIdx) {
  RpInfo info;
  info.colorCount = colorCount;
  info.colorIndex.assign(colorIdx, colorIdx + colorCount);
  info.finalLayouts.assign(finals, finals + attCount);
  if (depthIdx != VK_ATTACHMENT_UNUSED && depthIdx < attCount) {
    info.hasDepth = true;
    info.depthIndex = depthIdx;
    info.depthFormat = formats[depthIdx];
  }
  // The main scene pass: colour + normals + spec/gloss MRT with a depth-stencil buffer.
  info.isMain = colorCount >= 3 && info.hasDepth &&
                (info.depthFormat == VK_FORMAT_D24_UNORM_S8_UINT || info.depthFormat == VK_FORMAT_D32_SFLOAT_S8_UINT ||
                 info.depthFormat == VK_FORMAT_D32_SFLOAT);
  if (info.isMain) FEVERSCALER_LOG_N(8, "main scene render pass %p (colour x%u, depth fmt %d)", (void*)rp, colorCount, (int)info.depthFormat);
  std::unique_lock lk(g_mx);
  g_rps[rp] = std::move(info);
}

static VKAPI_ATTR VkResult VKAPI_CALL w_CreateRenderPass(VkDevice d, const VkRenderPassCreateInfo* ci,
                                                         const VkAllocationCallbacks* a, VkRenderPass* out) {
  VkResult r = vk.vkCreateRenderPass(d, ci, a, out);
  if (r == VK_SUCCESS && ci->subpassCount > 0) {
    std::vector<VkFormat> f(ci->attachmentCount);
    std::vector<VkImageLayout> fl(ci->attachmentCount);
    for (uint32_t i = 0; i < ci->attachmentCount; ++i) {
      f[i] = ci->pAttachments[i].format;
      fl[i] = ci->pAttachments[i].finalLayout;
    }
    const VkSubpassDescription& sp = ci->pSubpasses[0];
    std::vector<uint32_t> ci2(sp.colorAttachmentCount);
    for (uint32_t i = 0; i < sp.colorAttachmentCount; ++i) ci2[i] = sp.pColorAttachments[i].attachment;
    RecordRenderPass(*out, ci->attachmentCount, f.data(), fl.data(), sp.colorAttachmentCount, ci2.data(),
                     sp.pDepthStencilAttachment ? sp.pDepthStencilAttachment->attachment : VK_ATTACHMENT_UNUSED);
  }
  return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL w_CreateRenderPass2(VkDevice d, const VkRenderPassCreateInfo2* ci,
                                                          const VkAllocationCallbacks* a, VkRenderPass* out) {
  VkResult r = (vk.vkCreateRenderPass2KHR ? vk.vkCreateRenderPass2KHR : vk.vkCreateRenderPass2)(d, ci, a, out);
  if (r == VK_SUCCESS && ci->subpassCount > 0) {
    std::vector<VkFormat> f(ci->attachmentCount);
    std::vector<VkImageLayout> fl(ci->attachmentCount);
    for (uint32_t i = 0; i < ci->attachmentCount; ++i) {
      f[i] = ci->pAttachments[i].format;
      fl[i] = ci->pAttachments[i].finalLayout;
    }
    const VkSubpassDescription2& sp = ci->pSubpasses[0];
    std::vector<uint32_t> ci2(sp.colorAttachmentCount);
    for (uint32_t i = 0; i < sp.colorAttachmentCount; ++i) ci2[i] = sp.pColorAttachments[i].attachment;
    RecordRenderPass(*out, ci->attachmentCount, f.data(), fl.data(), sp.colorAttachmentCount, ci2.data(),
                     sp.pDepthStencilAttachment ? sp.pDepthStencilAttachment->attachment : VK_ATTACHMENT_UNUSED);
  }
  return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL w_CreateFramebuffer(VkDevice d, const VkFramebufferCreateInfo* ci,
                                                          const VkAllocationCallbacks* a, VkFramebuffer* out) {
  VkResult r = vk.vkCreateFramebuffer(d, ci, a, out);
  if (r == VK_SUCCESS) {
    FbInfo f{ci->renderPass, std::vector<VkImageView>(ci->pAttachments, ci->pAttachments + ci->attachmentCount),
             ci->width, ci->height};
    std::unique_lock lk(g_mx);
    g_fbs[*out] = std::move(f);
  }
  return r;
}
static VKAPI_ATTR void VKAPI_CALL w_DestroyFramebuffer(VkDevice d, VkFramebuffer f, const VkAllocationCallbacks* a) {
  {
    std::unique_lock lk(g_mx);
    g_fbs.erase(f);
  }
  vk.vkDestroyFramebuffer(d, f, a);
}
static VKAPI_ATTR VkResult VKAPI_CALL w_CreateImage(VkDevice d, const VkImageCreateInfo* ci,
                                                    const VkAllocationCallbacks* a, VkImage* out) {
  VkResult r = vk.vkCreateImage(d, ci, a, out);
  if (r == VK_SUCCESS) {
    std::unique_lock lk(g_mx);
    g_imgs[*out] = ImgInfo{ci->format, ci->extent.width, ci->extent.height, ci->usage, ci->samples, {}, ci->mipLevels};
  }
  return r;
}
static VKAPI_ATTR void VKAPI_CALL w_DestroyImage(VkDevice d, VkImage img, const VkAllocationCallbacks* a) {
  VkImage expected = img;
  g_worldDepth.compare_exchange_strong(expected, VK_NULL_HANDLE);
  {
    std::unique_lock lk(g_mx);
    g_imgs.erase(img);
  }
  vk.vkDestroyImage(d, img, a);
}
static VKAPI_ATTR VkResult VKAPI_CALL w_CreateImageView(VkDevice d, const VkImageViewCreateInfo* ci,
                                                        const VkAllocationCallbacks* a, VkImageView* out) {
  VkResult r = vk.vkCreateImageView(d, ci, a, out);
  if (r == VK_SUCCESS) {
    std::unique_lock lk(g_mx);
    g_views[*out] = ViewInfo{ci->image};
    auto im = g_imgs.find(ci->image);
    if (im != g_imgs.end()) im->second.views.push_back(*out);
  }
  return r;
}
static VKAPI_ATTR void VKAPI_CALL w_DestroyImageView(VkDevice d, VkImageView v, const VkAllocationCallbacks* a) {
  {
    std::unique_lock lk(g_mx);
    auto vi = g_views.find(v);
    if (vi != g_views.end()) {
      auto im = g_imgs.find(vi->second.image);
      if (im != g_imgs.end()) {
        auto& vs = im->second.views;
        for (size_t i = 0; i < vs.size(); ++i)
          if (vs[i] == v) {
            vs.erase(vs.begin() + i);
            break;
          }
      }
      g_views.erase(vi);
    }
  }
  vk.vkDestroyImageView(d, v, a);
}
static VKAPI_ATTR VkResult VKAPI_CALL w_CreateBuffer(VkDevice d, const VkBufferCreateInfo* ci,
                                                     const VkAllocationCallbacks* a, VkBuffer* out) {
  VkResult r = vk.vkCreateBuffer(d, ci, a, out);
  if (r == VK_SUCCESS) {
    std::unique_lock lk(g_mx);
    BufInfo& b = g_bufs[*out];
    b = BufInfo{};
    b.size = ci->size;
  }
  return r;
}
static VKAPI_ATTR void VKAPI_CALL w_DestroyBuffer(VkDevice d, VkBuffer b, const VkAllocationCallbacks* a) {
  {
    std::unique_lock lk(g_mx);
    g_bufs.erase(b);
  }
  vk.vkDestroyBuffer(d, b, a);
}
static VKAPI_ATTR VkResult VKAPI_CALL w_BindBufferMemory(VkDevice d, VkBuffer b, VkDeviceMemory m, VkDeviceSize off) {
  VkResult r = vk.vkBindBufferMemory(d, b, m, off);
  if (r == VK_SUCCESS) {
    std::unique_lock lk(g_mx);
    BufInfo& bi = g_bufs[b];
    bi.mem = m;
    bi.memOffset = off;
  }
  return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL w_BindBufferMemory2(VkDevice d, uint32_t n, const VkBindBufferMemoryInfo* infos) {
  VkResult r = (vk.vkBindBufferMemory2 ? vk.vkBindBufferMemory2 : vk.vkBindBufferMemory2KHR)(d, n, infos);
  if (r == VK_SUCCESS) {
    std::unique_lock lk(g_mx);
    for (uint32_t i = 0; i < n; ++i) {
      BufInfo& bi = g_bufs[infos[i].buffer];
      bi.mem = infos[i].memory;
      bi.memOffset = infos[i].memoryOffset;
    }
  }
  return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL w_AllocateMemory(VkDevice d, const VkMemoryAllocateInfo* ai,
                                                       const VkAllocationCallbacks* a, VkDeviceMemory* out) {
  VkResult r = vk.vkAllocateMemory(d, ai, a, out);
  if (r == VK_SUCCESS) {
    std::unique_lock lk(g_mx);
    MemInfo& m = g_mems[*out];
    m = MemInfo{};
    m.size = ai->allocationSize;
  }
  return r;
}
static VKAPI_ATTR void VKAPI_CALL w_FreeMemory(VkDevice d, VkDeviceMemory m, const VkAllocationCallbacks* a) {
  {
    std::unique_lock lk(g_mx);
    g_mems.erase(m);
  }
  vk.vkFreeMemory(d, m, a);
}
static VKAPI_ATTR VkResult VKAPI_CALL w_MapMemory(VkDevice d, VkDeviceMemory m, VkDeviceSize off, VkDeviceSize size,
                                                  VkMemoryMapFlags flags, void** pp) {
  VkResult r = vk.vkMapMemory(d, m, off, size, flags, pp);
  if (r == VK_SUCCESS && pp && *pp) {
    std::unique_lock lk(g_mx);
    MemInfo& mi = g_mems[m];
    mi.mapped = (uint8_t*)*pp;
    mi.mapOffset = off;
    mi.mapSize = size == VK_WHOLE_SIZE ? (mi.size > off ? mi.size - off : 0) : size;
  }
  return r;
}
static VKAPI_ATTR void VKAPI_CALL w_UnmapMemory(VkDevice d, VkDeviceMemory m) {
  {
    std::unique_lock lk(g_mx);
    auto it = g_mems.find(m);
    if (it != g_mems.end()) it->second.mapped = nullptr;
  }
  vk.vkUnmapMemory(d, m);
}
static VKAPI_ATTR void VKAPI_CALL w_UpdateDescriptorSets(VkDevice d, uint32_t nw, const VkWriteDescriptorSet* w,
                                                         uint32_t nc, const VkCopyDescriptorSet* c) {
  // Only the uniform buffers we read on the CPU: u_view (binding 10) and u_skinned (binding 11).
  for (uint32_t i = 0; i < nw; ++i) {
    const VkWriteDescriptorSet& wr = w[i];
    if ((wr.dstBinding != 10 && wr.dstBinding != 11) || !wr.pBufferInfo) continue;
    if (wr.descriptorType != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER &&
        wr.descriptorType != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)
      continue;
    UboBinding b{wr.pBufferInfo[0].buffer, wr.pBufferInfo[0].offset, wr.pBufferInfo[0].range,
                 wr.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC};
    std::unique_lock lk(g_mx);
    DescBindings& db = g_sets[wr.dstSet];
    (wr.dstBinding == 10 ? db.b10 : db.b11) = b;
  }
  vk.vkUpdateDescriptorSets(d, nw, w, nc, c);
  MipBiasOnUpdate(nw, w, nc, c);
}

// ---- pipelines -----------------------------------------------------------------------------------
static PipeInfo* AnalysePipeline(const VkGraphicsPipelineCreateInfo& ci) {
  auto* p = new PipeInfo();
  std::shared_ptr<SpirvInfo> vs;
  bool skinned = false, pos = false, terrain = false, billboard = false;
  {
    std::shared_lock lk(g_mx);
    auto rp = g_rps.find(ci.renderPass);
    p->mainPass = rp != g_rps.end() && rp->second.isMain;
    for (uint32_t s = 0; s < ci.stageCount; ++s) {
      auto m = g_modules.find(ci.pStages[s].module);
      if (m == g_modules.end()) continue;
      const SpirvInfo& info = *m->second;
      if (info.HasBinding("u_view")) p->usesView = true;
      if (info.HasBinding("u_blur")) p->isCompose = true;
      if (info.HasBinding("u_skinned")) skinned = true;
      if (info.HasBinding("u_pos")) billboard = true;
      if (info.HasBinding("u_terrain")) terrain = true;
      if (ci.pStages[s].stage == VK_SHADER_STAGE_VERTEX_BIT) vs = m->second;
    }
  }
  (void)pos;
  if (ci.pInputAssemblyState) {
    p->topology = ci.pInputAssemblyState->topology;
    p->primitiveRestart = ci.pInputAssemblyState->primitiveRestartEnable != VK_FALSE;
  }
  if (!p->mainPass || !vs || !ci.pVertexInputState || terrain || billboard) return p;
  const VkPipelineVertexInputStateCreateInfo& vi = *ci.pVertexInputState;
  auto attrAt = [&](int loc, PipeInfo::Attr* out, bool wantInstance) -> bool {
    if (loc < 0) return false;
    for (uint32_t i = 0; i < vi.vertexAttributeDescriptionCount; ++i) {
      const auto& a = vi.pVertexAttributeDescriptions[i];
      if ((int)a.location != loc) continue;
      for (uint32_t b = 0; b < vi.vertexBindingDescriptionCount; ++b) {
        const auto& bd = vi.pVertexBindingDescriptions[b];
        if (bd.binding != a.binding) continue;
        if ((bd.inputRate == VK_VERTEX_INPUT_RATE_INSTANCE) != wantInstance) return false;
        out->binding = a.binding;
        out->offset = a.offset;
        out->stride = bd.stride;
        out->format = a.format;
        return true;
      }
    }
    return false;
  };
  if (!attrAt(vs->InputLocation("attrPosition"), &p->pos, false)) return p;
  if (skinned) {
    if (attrAt(vs->InputLocation("attrInfluence"), &p->infl, false)) p->kind = PipeInfo::Skinned;
    return p;
  }
  int ml = vs->InputLocation("instAttrModel");
  PipeInfo::Attr cols[4];
  for (int c = 0; c < 4; ++c)
    if (ml < 0 || !attrAt(ml + c, &cols[c], true) || cols[c].binding != cols[0].binding) return p;
  p->modelBinding = cols[0].binding;
  p->modelStride = cols[0].stride;
  for (int c = 0; c < 4; ++c) p->modelOffs[c] = cols[c].offset;
  p->kind = PipeInfo::Mesh;
  return p;
}

static VKAPI_ATTR VkResult VKAPI_CALL w_CreateGraphicsPipelines(VkDevice d, VkPipelineCache cache, uint32_t n,
                                                                const VkGraphicsPipelineCreateInfo* cis,
                                                                const VkAllocationCallbacks* a, VkPipeline* out) {
  VkResult r = vk.vkCreateGraphicsPipelines(d, cache, n, cis, a, out);
  if (r == VK_SUCCESS) {
    for (uint32_t i = 0; i < n; ++i) {
      PipeInfo* p = AnalysePipeline(cis[i]);
      static int logged = 0;
      if (p->kind != PipeInfo::Other && logged < 12) {
        ++logged;
        Log("pipeline %p: %s pos(b%u off%u stride%u fmt%d) model(b%u stride%u) infl(b%u)", (void*)out[i],
            p->kind == PipeInfo::Mesh ? "mesh" : "skinned", p->pos.binding, p->pos.offset, p->pos.stride,
            (int)p->pos.format, p->modelBinding, p->modelStride, p->infl.binding);
      }
      // Twin without alpha-to-coverage (bound instead while DLSS runs; SR can be toggled any time).
      const VkPipelineMultisampleStateCreateInfo* ms = cis[i].pMultisampleState;
      if (!Cfg().keepAlphaToCoverage && ms && ms->alphaToCoverageEnable && ms->rasterizationSamples == VK_SAMPLE_COUNT_1_BIT) {
        VkGraphicsPipelineCreateInfo ci = cis[i];
        VkPipelineMultisampleStateCreateInfo ms2 = *ms;
        ms2.alphaToCoverageEnable = VK_FALSE;
        ci.pMultisampleState = &ms2;
        ci.flags &= ~VK_PIPELINE_CREATE_DERIVATIVE_BIT;
        ci.basePipelineHandle = VK_NULL_HANDLE;
        ci.basePipelineIndex = -1;
        // Preferred: the fragment shader patched for hashed alpha (partial coverage that DLSS averages).
        bool hashed = false;
        if (Cfg().hashedAlpha) {
          std::vector<VkPipelineShaderStageCreateInfo> stages(ci.pStages, ci.pStages + ci.stageCount);
          for (auto& st : stages) {
            if (st.stage != VK_SHADER_STAGE_FRAGMENT_BIT) continue;
            std::vector<uint32_t> patched;
            {
              std::shared_lock lk(g_mx);
              auto m = g_modules.find(st.module);
              if (m != g_modules.end() && !m->second->code.empty())
                patched = PatchHashedAlpha(m->second->code.data(), m->second->code.size() * 4);
            }
            if (patched.empty()) break;
            VkShaderModuleCreateInfo mci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            mci.codeSize = patched.size() * 4;
            mci.pCode = patched.data();
            VkShaderModule mod = VK_NULL_HANDLE;
            if (vk.vkCreateShaderModule(d, &mci, nullptr, &mod) != VK_SUCCESS) break;
            st.module = mod;
            VkGraphicsPipelineCreateInfo hci = ci;
            hci.pStages = stages.data();
            hashed = vk.vkCreateGraphicsPipelines(d, cache, 1, &hci, a, &p->noA2C) == VK_SUCCESS;
            if (!hashed) p->noA2C = VK_NULL_HANDLE;
            vk.vkDestroyShaderModule(d, mod, nullptr);
            break;
          }
        }
        if (!hashed && vk.vkCreateGraphicsPipelines(d, cache, 1, &ci, a, &p->noA2C) != VK_SUCCESS) p->noA2C = VK_NULL_HANDLE;
        static std::atomic<int> twins{0}, hashedTwins{0};
        if (hashed) ++hashedTwins;
        if (p->noA2C && ++twins % 10 == 1)
          Log("alpha-to-coverage: %d pipelines have a DLSS twin (%d with hashed alpha)", twins.load(), hashedTwins.load());
      }
      // SSR changes also need to disappear immediately when SR is off or has no usable output.
      std::vector<VkPipelineShaderStageCreateInfo> stages(cis[i].pStages, cis[i].pStages + cis[i].stageCount);
      std::vector<VkShaderModule> temporary;
      for (auto& stage : stages) {
        std::vector<uint32_t> code;
        {
          std::shared_lock lk(g_mx);
          auto shader = g_modules.find(stage.module);
          if (shader != g_modules.end()) code = shader->second->srCode;
        }
        if (code.empty()) continue;
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = code.size() * sizeof(uint32_t);
        ci.pCode = code.data();
        VkShaderModule shader = VK_NULL_HANDLE;
        if (vk.vkCreateShaderModule(d, &ci, nullptr, &shader) != VK_SUCCESS) continue;
        temporary.push_back(shader);
        stage.module = shader;
      }
      if (!temporary.empty()) {
        VkGraphicsPipelineCreateInfo ci = cis[i];
        ci.pStages = stages.data();
        ci.flags &= ~VK_PIPELINE_CREATE_DERIVATIVE_BIT;
        ci.basePipelineHandle = VK_NULL_HANDLE;
        ci.basePipelineIndex = -1;
        if (vk.vkCreateGraphicsPipelines(d, cache, 1, &ci, a, &p->srSsr) != VK_SUCCESS) p->srSsr = VK_NULL_HANDLE;
        for (VkShaderModule shader : temporary) vk.vkDestroyShaderModule(d, shader, nullptr);
      }
      std::unique_lock lk(g_mx);
      g_pipes[out[i]] = p;  // PipeInfo is never freed: draw records may still point at it
    }
  }
  return r;
}
static VKAPI_ATTR void VKAPI_CALL w_DestroyPipeline(VkDevice d, VkPipeline p, const VkAllocationCallbacks* a) {
  VkPipeline twin = VK_NULL_HANDLE;
  VkPipeline ssr = VK_NULL_HANDLE;
  {
    std::unique_lock lk(g_mx);
    auto it = g_pipes.find(p);
    if (it != g_pipes.end()) {
      twin = it->second->noA2C;
      it->second->noA2C = VK_NULL_HANDLE;
      ssr = it->second->srSsr;
      it->second->srSsr = VK_NULL_HANDLE;
      g_pipes.erase(it);
    }
  }
  if (twin) vk.vkDestroyPipeline(d, twin, a);
  if (ssr) vk.vkDestroyPipeline(d, ssr, a);
  vk.vkDestroyPipeline(d, p, a);
}

// ---- command recording -----------------------------------------------------------------------------
static VKAPI_ATTR VkResult VKAPI_CALL w_BeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo* bi) {
  GetCbState(cb)->Reset();
  return vk.vkBeginCommandBuffer(cb, bi);
}
static VKAPI_ATTR VkResult VKAPI_CALL w_ResetCommandBuffer(VkCommandBuffer cb, VkCommandBufferResetFlags f) {
  GetCbState(cb)->Reset();
  return vk.vkResetCommandBuffer(cb, f);
}
static VKAPI_ATTR void VKAPI_CALL w_CmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p) {
  if (bp == VK_PIPELINE_BIND_POINT_GRAPHICS) {
    CbState* st = GetCbState(cb);
    st->pipeline = p;
    std::shared_lock lk(g_mx);
    auto it = g_pipes.find(p);
    st->pipe = it != g_pipes.end() ? it->second : nullptr;
    if (st->pipe && st->pipe->isCompose && st->inMainPass) st->afterCompose = true;
    if (st->pipe && st->pipe->noA2C && st->inMainPass && SrActive()) {
      vk.vkCmdBindPipeline(cb, bp, st->pipe->noA2C);
      return;
    }
    if (st->pipe && st->pipe->srSsr && st->worldView && SrActive()) {
      vk.vkCmdBindPipeline(cb, bp, st->pipe->srSsr);
      return;
    }
  }
  vk.vkCmdBindPipeline(cb, bp, p);
}
static VKAPI_ATTR void VKAPI_CALL w_CmdBindVertexBuffers(VkCommandBuffer cb, uint32_t first, uint32_t n,
                                                         const VkBuffer* bufs, const VkDeviceSize* offs) {
  CbState* st = GetCbState(cb);
  for (uint32_t i = 0; i < n && first + i < 16; ++i) {
    st->vb[first + i] = bufs[i];
    st->vbOff[first + i] = offs[i];
  }
  vk.vkCmdBindVertexBuffers(cb, first, n, bufs, offs);
}
static VKAPI_ATTR void VKAPI_CALL w_CmdBindIndexBuffer(VkCommandBuffer cb, VkBuffer b, VkDeviceSize off, VkIndexType t) {
  CbState* st = GetCbState(cb);
  st->ib = b;
  st->ibOff = off;
  st->ibType = t;
  vk.vkCmdBindIndexBuffer(cb, b, off, t);
}
static VKAPI_ATTR void VKAPI_CALL w_CmdBindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint bp,
                                                          VkPipelineLayout layout, uint32_t first, uint32_t n,
                                                          const VkDescriptorSet* sets, uint32_t nd, const uint32_t* dyn) {
  if (bp == VK_PIPELINE_BIND_POINT_GRAPHICS) {
    CbState* st = GetCbState(cb);
    for (uint32_t i = 0; i < n && first + i < 8; ++i) st->sets[first + i] = sets[i];
    // u_skinned is the only dynamic uniform buffer we read; it lives in set 3 (last set bound).
    if (first <= 3 && first + n > 3) st->dyn3 = nd ? dyn[nd - 1] : 0;
    // While DLSS upscales the world: copies whose texture samplers have the mip bias. In the UI pass
    // after a DLSS evaluate: copies that sample the DLSS output instead of the scene image.
    if (n <= 8 && ((st->inMainPass && SrActive()) || st->sceneOut)) {
      VkDescriptorSet copies[8];
      bool any = false;
      for (uint32_t i = 0; i < n; ++i) {
        copies[i] = st->sceneOut ? SceneSet(sets[i], st->sceneViews, st->sceneViewCount, st->sceneOut) : MipBiasSet(sets[i]);
        any |= copies[i] != sets[i];
      }
      if (any) {
        st->sceneShown |= st->sceneOut != VK_NULL_HANDLE;
        vk.vkCmdBindDescriptorSets(cb, bp, layout, first, n, copies, nd, dyn);
        return;
      }
    }
  }
  vk.vkCmdBindDescriptorSets(cb, bp, layout, first, n, sets, nd, dyn);
}

// A full-view image: at least the render size of this frame's 3D pass (DLSS presets go down to a
// third of the screen), or half the screen before the 3D pass size is known.
static bool SceneSized(const CbState* st, uint32_t w, uint32_t h) {
  if (st->mainW && st->mainH) return w >= st->mainW && h >= st->mainH;
  return 2 * w >= SwapchainExtent().width;
}

// The world view: its 3D passes are window size x render scale. Vehicle / station window previews
// run the same passes at their own small size and must not be taken for the world (DLSS input,
// motion vectors, camera). Right after a scale change the previous world size still counts.
static std::atomic<uint32_t> g_worldW{0}, g_worldH{0};
static std::atomic<uint64_t> g_worldTick{0};
uint64_t LastWorldPassTick() { return g_worldTick.load(); }

static bool IsWorldView(uint32_t w, uint32_t h) {
  VkExtent2D se = SwapchainExtent();
  float s = GameScaleOverride();
  if (s <= 0.0f) s = GameSliderScale();
  auto sameSize = [](uint32_t a, uint32_t b) { return std::abs((int)a - (int)b) <= 1; };
  if (se.width && s > 0.0f) {
    uint32_t ew = (uint32_t)std::floor(se.width * s + 0.5f), eh = (uint32_t)std::floor(se.height * s + 0.5f);
    if (sameSize(w, ew) && sameSize(h, eh)) return true;
    return sameSize(w, g_worldW.load()) && sameSize(h, g_worldH.load());
  }
  // No reliable world size: do not let a preview define the camera or DLSS inputs.
  return false;
}

static void OnBeginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo* rb) {
  CbState* st = GetCbState(cb);
  VkImage att0 = VK_NULL_HANDLE, depth = VK_NULL_HANDLE;
  bool isMain = false, single = false, swap = false;
  uint32_t w = 0, h = 0;
  VkImageLayout final0 = VK_IMAGE_LAYOUT_UNDEFINED;
  VkFormat depthFmt = VK_FORMAT_UNDEFINED;
  VkSampleCountFlagBits depthSamples = VkSampleCountFlagBits(0);
  {
    std::shared_lock lk(g_mx);
    auto fb = g_fbs.find(rb->framebuffer);
    if (fb == g_fbs.end()) return;
    auto rp = g_rps.find(rb->renderPass);  // the pass actually begun (layouts/load ops can differ)
    if (rp == g_rps.end()) rp = g_rps.find(fb->second.rp);
    if (rp == g_rps.end()) return;
    const RpInfo& ri = rp->second;
    w = fb->second.w;
    h = fb->second.h;
    isMain = ri.isMain;
    single = ri.colorCount == 1;
    auto viewImage = [&](uint32_t idx) -> VkImage {
      if (idx >= fb->second.views.size()) return VK_NULL_HANDLE;
      auto v = g_views.find(fb->second.views[idx]);
      return v != g_views.end() ? v->second.image : VK_NULL_HANDLE;
    };
    if (ri.colorCount) {
      att0 = viewImage(ri.colorIndex[0]);
      final0 = ri.finalLayouts[ri.colorIndex[0]];
    }
    if (ri.hasDepth) {
      depth = viewImage(ri.depthIndex);
      depthFmt = ri.depthFormat;
      auto image = g_imgs.find(depth);
      if (image != g_imgs.end()) depthSamples = image->second.samples;
    }
    // Swapchain target: an image from vkGetSwapchainImagesKHR, or one the game never created
    // itself (with DLSS-G active the game renders into Streamline's proxy back buffers, which
    // Streamline creates behind our wrappers).
    bool atSwapSize = w == g_swapExtent.width && h == g_swapExtent.height;
    swap = att0 && single && atSwapSize && (g_swapImages.count(att0) || !g_imgs.count(att0));
  }
  if (isMain) st->worldView = IsWorldView(w, h);
  if (isMain && !st->worldView) {
    st->inMainPass = false;  // a preview window's 3D view: not ours to touch
    FEVERSCALER_LOG_N(3, "3D pass %ux%u is not the world view (preview window?) - ignored", w, h);
  } else if (isMain) {
    g_worldDepth = depth;
    g_worldW = w;
    g_worldH = h;
    g_worldTick = GetTickCount64();
    st->inMainPass = true;
    st->hasMainPass = true;
    st->mainDepth = depth;
    st->mainDepthFormat = depthFmt;
    st->mainSamples = depthSamples;
    st->mainW = w;
    st->mainH = h;
  } else if (swap) {
    // First swapchain pass after the scene: the UI pass. The frame's DLSS-G inputs are built at
    // the submit that carries it; the HUD-less copy needs a scene-colour source in this buffer.
    if (!st->uiPassSeen) {
      st->uiPassSeen = true;
      if (st->candImage) OnUiPassBegin(cb, st);
      else if (st->afterCompose) FEVERSCALER_LOG_N(3, "UI pass with no scene-colour source before it: no HUD-less copy");
    }
  } else if (single && att0 && st->afterCompose && !st->uiPassSeen && SceneSized(st, w, h)) {
    // A tonemapped full-view colour image written after compose: FSR1 output, or the game's
    // render-resolution scene texture that the UI pass stretches to the screen.
    st->candImage = att0;
    st->candLayout = final0;
    st->candW = w;
    st->candH = h;
  }
}
static VKAPI_ATTR void VKAPI_CALL w_CmdBeginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo* rb,
                                                       VkSubpassContents c) {
  OnBeginRenderPass(cb, rb);
  vk.vkCmdBeginRenderPass(cb, rb, c);
}
static VKAPI_ATTR void VKAPI_CALL w_CmdBeginRenderPass2(VkCommandBuffer cb, const VkRenderPassBeginInfo* rb,
                                                        const VkSubpassBeginInfo* sb) {
  OnBeginRenderPass(cb, rb);
  (vk.vkCmdBeginRenderPass2KHR ? vk.vkCmdBeginRenderPass2KHR : vk.vkCmdBeginRenderPass2)(cb, rb, sb);
}
static VKAPI_ATTR void VKAPI_CALL w_CmdEndRenderPass(VkCommandBuffer cb) {
  GetCbState(cb)->inMainPass = false;
  vk.vkCmdEndRenderPass(cb);
}
static VKAPI_ATTR void VKAPI_CALL w_CmdEndRenderPass2(VkCommandBuffer cb, const VkSubpassEndInfo* si) {
  GetCbState(cb)->inMainPass = false;
  (vk.vkCmdEndRenderPass2KHR ? vk.vkCmdEndRenderPass2KHR : vk.vkCmdEndRenderPass2)(cb, si);
}

static inline void RecordDraw(CbState* st, bool indexed, uint32_t count, uint32_t inst, uint32_t first, int32_t vo,
                              uint32_t firstInst) {
  const PipeInfo* p = st->pipe;
  if (!st->viewSet && st->inMainPass && !st->afterCompose && p && p->usesView && st->sets[0]) st->viewSet = st->sets[0];
  if (p && p->usesView && st->sets[0] && st->sets[0] != st->viewSet && st->otherViewCount < 16) {
    bool known = false;
    for (uint32_t i = 0; i < st->otherViewCount && !known; ++i) known = st->otherViewSets[i] == st->sets[0];
    if (!known) st->otherViewSets[st->otherViewCount++] = st->sets[0];
  }
  if (!st->inMainPass || st->afterCompose || !p || p->kind == PipeInfo::Other || inst == 0) return;
  DrawRec d;
  d.pipe = p;
  d.pipeline = st->pipeline;
  d.posBuf = st->vb[p->pos.binding & 15];
  d.posOff = st->vbOff[p->pos.binding & 15];
  if (p->kind == PipeInfo::Skinned) {
    d.inflBuf = st->vb[p->infl.binding & 15];
    d.inflOff = st->vbOff[p->infl.binding & 15];
  } else {
    d.modelBuf = st->vb[p->modelBinding & 15];
    d.modelOff = st->vbOff[p->modelBinding & 15];
  }
  d.indexed = indexed;
  if (indexed) {
    d.idxBuf = st->ib;
    d.idxOff = st->ibOff;
    d.idxType = st->ibType;
  }
  d.count = count;
  d.instanceCount = inst;
  d.first = first;
  d.vertexOffset = vo;
  d.firstInstance = firstInst;
  d.set0 = st->sets[0];
  d.set3 = st->sets[3];
  d.dyn3 = st->dyn3;
  if (!d.posBuf) return;
  st->draws.push_back(d);
}
static VKAPI_ATTR void VKAPI_CALL w_CmdDraw(VkCommandBuffer cb, uint32_t vc, uint32_t ic, uint32_t fv, uint32_t fi) {
  RecordDraw(GetCbState(cb), false, vc, ic, fv, 0, fi);
  vk.vkCmdDraw(cb, vc, ic, fv, fi);
}
static VKAPI_ATTR void VKAPI_CALL w_CmdDrawIndexed(VkCommandBuffer cb, uint32_t ixc, uint32_t ic, uint32_t fix, int32_t vo,
                                                   uint32_t fi) {
  RecordDraw(GetCbState(cb), true, ixc, ic, fix, vo, fi);
  vk.vkCmdDrawIndexed(cb, ixc, ic, fix, vo, fi);
}

static void NoteColorWrite(CbState* st, VkImage dst, VkImageLayout layout) {
  if (!st->afterCompose || st->uiPassSeen) return;  // only tonemapped images, before the UI
  VkFormat f;
  uint32_t w, h;
  if (!GetImageInfo(dst, &f, &w, &h)) return;
  if (f >= VK_FORMAT_D16_UNORM && f <= VK_FORMAT_D32_SFLOAT_S8_UINT) return;
  if (st->mainW && w == st->mainW && h == st->mainH) {  // the scene copy FSR1 reads: DLSS input
    st->srIn = dst;
    st->srInLayout = layout;
    st->srInW = w;
    st->srInH = h;
  }
  VkExtent2D se = SwapchainExtent();
  (void)se;
  if (!SceneSized(st, w, h) || IsSwapchainImage(dst)) return;
  st->candImage = dst;
  st->candLayout = layout;
  st->candW = w;
  st->candH = h;
}
static VKAPI_ATTR void VKAPI_CALL w_CmdBlitImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst,
                                                 VkImageLayout dl, uint32_t n, const VkImageBlit* r, VkFilter f) {
  NoteColorWrite(GetCbState(cb), dst, dl);
  vk.vkCmdBlitImage(cb, src, sl, dst, dl, n, r, f);
}
static VKAPI_ATTR void VKAPI_CALL w_CmdCopyImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst,
                                                 VkImageLayout dl, uint32_t n, const VkImageCopy* r) {
  NoteColorWrite(GetCbState(cb), dst, dl);
  vk.vkCmdCopyImage(cb, src, sl, dst, dl, n, r);
}
static VKAPI_ATTR void VKAPI_CALL w_CmdPipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags ss, VkPipelineStageFlags ds,
                                                       VkDependencyFlags df, uint32_t nm, const VkMemoryBarrier* mb,
                                                       uint32_t nb, const VkBufferMemoryBarrier* bb, uint32_t ni,
                                                       const VkImageMemoryBarrier* ib) {
  CbState* st = GetCbState(cb);
  if (st->candImage || st->srIn)
    for (uint32_t i = 0; i < ni; ++i) {
      if (ib[i].image == st->candImage) st->candLayout = ib[i].newLayout;
      if (ib[i].image == st->srIn) st->srInLayout = ib[i].newLayout;
    }
  vk.vkCmdPipelineBarrier(cb, ss, ds, df, nm, mb, nb, bb, ni, ib);
}

// ---- wrapper table -----------------------------------------------------------------------------------
PFN_vkVoidFunction LookupDeviceWrapper(const char* name, PFN_vkVoidFunction) {
  if (PFN_vkVoidFunction f = FrameWrapper(name)) return f;
  if (PFN_vkVoidFunction f = MipBiasWrapper(name)) return f;
  struct Entry {
    const char* name;
    PFN_vkVoidFunction fn;
  };
  static const Entry table[] = {
      {"vkCreateShaderModule", (PFN_vkVoidFunction)w_CreateShaderModule},
      {"vkDestroyShaderModule", (PFN_vkVoidFunction)w_DestroyShaderModule},
      {"vkCreateRenderPass", (PFN_vkVoidFunction)w_CreateRenderPass},
      {"vkCreateRenderPass2", (PFN_vkVoidFunction)w_CreateRenderPass2},
      {"vkCreateRenderPass2KHR", (PFN_vkVoidFunction)w_CreateRenderPass2},
      {"vkCreateFramebuffer", (PFN_vkVoidFunction)w_CreateFramebuffer},
      {"vkDestroyFramebuffer", (PFN_vkVoidFunction)w_DestroyFramebuffer},
      {"vkCreateImage", (PFN_vkVoidFunction)w_CreateImage},
      {"vkDestroyImage", (PFN_vkVoidFunction)w_DestroyImage},
      {"vkCreateImageView", (PFN_vkVoidFunction)w_CreateImageView},
      {"vkDestroyImageView", (PFN_vkVoidFunction)w_DestroyImageView},
      {"vkCreateBuffer", (PFN_vkVoidFunction)w_CreateBuffer},
      {"vkDestroyBuffer", (PFN_vkVoidFunction)w_DestroyBuffer},
      {"vkBindBufferMemory", (PFN_vkVoidFunction)w_BindBufferMemory},
      {"vkBindBufferMemory2", (PFN_vkVoidFunction)w_BindBufferMemory2},
      {"vkBindBufferMemory2KHR", (PFN_vkVoidFunction)w_BindBufferMemory2},
      {"vkAllocateMemory", (PFN_vkVoidFunction)w_AllocateMemory},
      {"vkFreeMemory", (PFN_vkVoidFunction)w_FreeMemory},
      {"vkMapMemory", (PFN_vkVoidFunction)w_MapMemory},
      {"vkUnmapMemory", (PFN_vkVoidFunction)w_UnmapMemory},
      {"vkUpdateDescriptorSets", (PFN_vkVoidFunction)w_UpdateDescriptorSets},
      {"vkCreateGraphicsPipelines", (PFN_vkVoidFunction)w_CreateGraphicsPipelines},
      {"vkDestroyPipeline", (PFN_vkVoidFunction)w_DestroyPipeline},
      {"vkBeginCommandBuffer", (PFN_vkVoidFunction)w_BeginCommandBuffer},
      {"vkResetCommandBuffer", (PFN_vkVoidFunction)w_ResetCommandBuffer},
      {"vkCmdBindPipeline", (PFN_vkVoidFunction)w_CmdBindPipeline},
      {"vkCmdBindVertexBuffers", (PFN_vkVoidFunction)w_CmdBindVertexBuffers},
      {"vkCmdBindIndexBuffer", (PFN_vkVoidFunction)w_CmdBindIndexBuffer},
      {"vkCmdBindDescriptorSets", (PFN_vkVoidFunction)w_CmdBindDescriptorSets},
      {"vkCmdBeginRenderPass", (PFN_vkVoidFunction)w_CmdBeginRenderPass},
      {"vkCmdBeginRenderPass2", (PFN_vkVoidFunction)w_CmdBeginRenderPass2},
      {"vkCmdBeginRenderPass2KHR", (PFN_vkVoidFunction)w_CmdBeginRenderPass2},
      {"vkCmdEndRenderPass", (PFN_vkVoidFunction)w_CmdEndRenderPass},
      {"vkCmdEndRenderPass2", (PFN_vkVoidFunction)w_CmdEndRenderPass2},
      {"vkCmdEndRenderPass2KHR", (PFN_vkVoidFunction)w_CmdEndRenderPass2},
      {"vkCmdDraw", (PFN_vkVoidFunction)w_CmdDraw},
      {"vkCmdDrawIndexed", (PFN_vkVoidFunction)w_CmdDrawIndexed},
      {"vkCmdBlitImage", (PFN_vkVoidFunction)w_CmdBlitImage},
      {"vkCmdCopyImage", (PFN_vkVoidFunction)w_CmdCopyImage},
      {"vkCmdPipelineBarrier", (PFN_vkVoidFunction)w_CmdPipelineBarrier},
  };
  for (const Entry& e : table)
    if (!strcmp(e.name, name)) return e.fn;
  return nullptr;
}
}  // namespace feverscaler
