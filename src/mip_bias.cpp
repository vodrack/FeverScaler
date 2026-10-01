// Texture mip bias while DLSS upscales (see mip_bias.h).
//
// The bias is log2(render width / output width) on top of the game's own sampler bias, so textures
// are sampled as at the output resolution. Only descriptors that pair a mip-mapped sampler with a
// mip-mapped asset texture get it: in Vulkan the sampler bias also shifts explicit-LOD reads, which
// the game makes of render targets (environment maps, the HDR blur chain). Separate sampler
// descriptors are biased as well; TF3 uses them only for the terrain and grass texture arrays.
#include "mip_bias.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include "hooks.h"
#include "log.h"
#include "tracker.h"

namespace feverscaler {
namespace {
constexpr float kSteps = 32.0f;        // bias resolution: 1/32 mip level
constexpr uint64_t kRetireFrames = 8;  // presents between retiring a copy and destroying it

struct Sampler {
  VkSamplerCreateInfo ci{};
  bool biasable = false;
  std::unordered_map<int, VkSampler> twins;  // bias steps -> our copy with that bias added
};
struct Layout {
  bool supported = true, alive = true;
  VkDescriptorPoolCreateFlags poolFlags = 0;
  std::vector<VkDescriptorSetLayoutBinding> bindings;  // sorted by binding number
  std::vector<VkDescriptorPool> pools;                 // ours, for copies of sets with this layout
};
struct Slot {
  VkDescriptorImageInfo image{};
  VkDescriptorBufferInfo buffer{};
  VkBufferView texel = VK_NULL_HANDLE;
  bool written = false, bias = false;
};
struct Copy {
  int steps = 0;
  VkDescriptorSet set = VK_NULL_HANDLE;
  VkDescriptorPool pool = VK_NULL_HANDLE;
};
struct Set {
  std::shared_ptr<Layout> layout;
  VkDescriptorSetLayout handle = VK_NULL_HANDLE;
  VkDescriptorPool pool = VK_NULL_HANDLE;  // the game's
  std::vector<std::vector<Slot>> slots;    // per layout binding, per array element
  bool supported = true, biased = false;
  std::vector<Copy> copies;
};
struct Retired {
  uint64_t frame;
  VkDescriptorPool pool;
  VkDescriptorSet set;
  VkSampler sampler;
};

std::shared_mutex g_mx;  // guards everything below
std::unordered_map<VkSampler, Sampler> g_samplers;
std::unordered_map<VkDescriptorSetLayout, std::shared_ptr<Layout>> g_layouts;
std::unordered_map<VkDescriptorSet, Set> g_sets;
std::unordered_map<VkDescriptorPool, std::vector<VkDescriptorSet>> g_poolSets;  // the game's pools
std::vector<Retired> g_retired;
uint64_t g_frame = 0;
float g_maxBias = 0;
std::atomic<int> g_steps{0};  // current bias in 1/kSteps mip levels, 0 = none

enum class Kind { Image, Buffer, Texel, Other };
Kind KindOf(VkDescriptorType t) {
  switch (t) {
    case VK_DESCRIPTOR_TYPE_SAMPLER:
    case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
    case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
    case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
    case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT: return Kind::Image;
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: return Kind::Buffer;
    case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
    case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: return Kind::Texel;
    default: return Kind::Other;
  }
}
bool HoldsSampler(VkDescriptorType t) {
  return t == VK_DESCRIPTOR_TYPE_SAMPLER || t == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
}

// Calls fn(binding index, array element, i) for `count` descriptors from (binding, element),
// continuing into the following bindings as Vulkan's consecutive binding updates do.
template <class F>
bool Walk(const Layout& l, uint32_t binding, uint32_t element, uint32_t count, F fn) {
  size_t b = 0;
  while (b < l.bindings.size() && l.bindings[b].binding != binding) ++b;
  for (uint32_t i = 0; i < count; ++i, ++element) {
    while (b < l.bindings.size() && element >= l.bindings[b].descriptorCount) {
      element -= l.bindings[b].descriptorCount;
      ++b;
    }
    if (b >= l.bindings.size()) return false;
    fn(b, element, i);
  }
  return true;
}

bool BiasedSlot(VkDescriptorType type, const VkDescriptorImageInfo& ii) {
  if (!HoldsSampler(type)) return false;
  auto s = g_samplers.find(ii.sampler);
  if (s == g_samplers.end() || !s->second.biasable) return false;
  return type == VK_DESCRIPTOR_TYPE_SAMPLER || IsMipmappedTexture(ii.imageView);
}

void Recount(Set& s) {
  s.biased = false;
  for (const auto& binding : s.slots)
    for (const Slot& slot : binding) s.biased |= slot.written && slot.bias;
}

void RetireCopies(Set& s) {
  for (const Copy& c : s.copies) g_retired.push_back({g_frame + kRetireFrames, c.pool, c.set, VK_NULL_HANDLE});
  s.copies.clear();
}

void Forget(VkDescriptorSet set) {
  auto it = g_sets.find(set);
  if (it == g_sets.end()) return;
  RetireCopies(it->second);
  g_sets.erase(it);
}

VkSampler Twin(VkSampler original, int steps) {
  auto it = g_samplers.find(original);
  if (it == g_samplers.end()) return VK_NULL_HANDLE;
  VkSampler& twin = it->second.twins[steps];
  if (!twin) {
    if (!g_maxBias) {
      VkPhysicalDeviceProperties p{};
      vk.vkGetPhysicalDeviceProperties(gPhysicalDevice, &p);
      g_maxBias = p.limits.maxSamplerLodBias;
    }
    VkSamplerCreateInfo ci = it->second.ci;
    ci.mipLodBias = std::clamp(ci.mipLodBias + (float)steps / kSteps, -g_maxBias, g_maxBias);
    if (vk.vkCreateSampler(gDevice, &ci, nullptr, &twin) != VK_SUCCESS) twin = VK_NULL_HANDLE;
  }
  return twin;
}

VkDescriptorPool NewPool(const Layout& l) {
  uint32_t perSet = 0;
  for (const auto& b : l.bindings) perSet += b.descriptorCount;
  uint32_t sets = std::clamp(4096u / std::max(perSet, 1u), 4u, 256u);
  std::vector<VkDescriptorPoolSize> sizes;
  for (const auto& b : l.bindings) {
    if (!b.descriptorCount) continue;
    auto same = std::find_if(sizes.begin(), sizes.end(), [&](const VkDescriptorPoolSize& s) { return s.type == b.descriptorType; });
    if (same != sizes.end()) same->descriptorCount += b.descriptorCount * sets;
    else sizes.push_back({b.descriptorType, b.descriptorCount * sets});
  }
  VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  ci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT | l.poolFlags;
  ci.maxSets = sets;
  ci.poolSizeCount = (uint32_t)sizes.size();
  ci.pPoolSizes = sizes.data();
  VkDescriptorPool pool = VK_NULL_HANDLE;
  if (vk.vkCreateDescriptorPool(gDevice, &ci, nullptr, &pool) != VK_SUCCESS) return VK_NULL_HANDLE;
  return pool;
}

bool Allocate(Layout& l, VkDescriptorSetLayout handle, Copy* c) {
  VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  ai.descriptorSetCount = 1;
  ai.pSetLayouts = &handle;
  for (auto p = l.pools.rbegin(); p != l.pools.rend(); ++p) {
    ai.descriptorPool = *p;
    if (vk.vkAllocateDescriptorSets(gDevice, &ai, &c->set) == VK_SUCCESS) {
      c->pool = *p;
      return true;
    }
  }
  VkDescriptorPool pool = NewPool(l);
  if (!pool) return false;
  l.pools.push_back(pool);
  ai.descriptorPool = pool;
  if (vk.vkAllocateDescriptorSets(gDevice, &ai, &c->set) != VK_SUCCESS) return false;
  c->pool = pool;
  return true;
}

// Our copy of the game's set: every written descriptor, with the biased samplers swapped for twins.
bool MakeCopy(Set& s, Copy* c) {
  Layout& l = *s.layout;
  if (!Allocate(l, s.handle, c)) {
    FEVERSCALER_LOG_N(3, "mip bias: no descriptor set for a copy - that set stays unbiased");
    return false;
  }
  size_t n = 0;
  for (const auto& binding : s.slots)
    for (const Slot& slot : binding) n += slot.written;
  std::vector<VkWriteDescriptorSet> writes;
  std::vector<VkDescriptorImageInfo> images;
  std::vector<VkDescriptorBufferInfo> buffers;
  std::vector<VkBufferView> texels;
  writes.reserve(n);
  images.reserve(n);  // the writes point into these: no reallocation
  buffers.reserve(n);
  texels.reserve(n);
  for (size_t b = 0; b < s.slots.size(); ++b)
    for (uint32_t e = 0; e < (uint32_t)s.slots[b].size(); ++e) {
      const Slot& slot = s.slots[b][e];
      if (!slot.written) continue;
      VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      w.dstSet = c->set;
      w.dstBinding = l.bindings[b].binding;
      w.dstArrayElement = e;
      w.descriptorCount = 1;
      w.descriptorType = l.bindings[b].descriptorType;
      switch (KindOf(w.descriptorType)) {
        case Kind::Image:
          images.push_back(slot.image);
          if (slot.bias && !(images.back().sampler = Twin(slot.image.sampler, c->steps))) {
            vk.vkFreeDescriptorSets(gDevice, c->pool, 1, &c->set);
            FEVERSCALER_LOG_N(3, "mip bias: sampler copy failed - that set stays unbiased");
            return false;
          }
          w.pImageInfo = &images.back();
          break;
        case Kind::Buffer:
          buffers.push_back(slot.buffer);
          w.pBufferInfo = &buffers.back();
          break;
        case Kind::Texel:
          texels.push_back(slot.texel);
          w.pTexelBufferView = &texels.back();
          break;
        case Kind::Other: continue;
      }
      writes.push_back(w);
    }
  vk.vkUpdateDescriptorSets(gDevice, (uint32_t)writes.size(), writes.data(), 0, nullptr);
  FEVERSCALER_LOG_N(1, "mip bias: first descriptor set copy (%zu descriptors)", writes.size());
  return true;
}

// ---- wrappers ------------------------------------------------------------------------------------------
VKAPI_ATTR VkResult VKAPI_CALL w_CreateSampler(VkDevice d, const VkSamplerCreateInfo* ci, const VkAllocationCallbacks* a,
                                               VkSampler* out) {
  VkResult r = vk.vkCreateSampler(d, ci, a, out);
  if (r == VK_SUCCESS) {
    Sampler s;
    s.ci = *ci;
    s.biasable = !ci->pNext && ci->maxLod > ci->minLod && !ci->compareEnable && !ci->unnormalizedCoordinates;
    std::unique_lock lk(g_mx);
    g_samplers[*out] = std::move(s);
  }
  return r;
}
VKAPI_ATTR void VKAPI_CALL w_DestroySampler(VkDevice d, VkSampler sampler, const VkAllocationCallbacks* a) {
  {
    std::unique_lock lk(g_mx);
    auto it = g_samplers.find(sampler);
    if (it != g_samplers.end()) {
      for (const auto& twin : it->second.twins)
        if (twin.second) g_retired.push_back({g_frame + kRetireFrames, VK_NULL_HANDLE, VK_NULL_HANDLE, twin.second});
      g_samplers.erase(it);
    }
  }
  vk.vkDestroySampler(d, sampler, a);
}
VKAPI_ATTR VkResult VKAPI_CALL w_CreateDescriptorSetLayout(VkDevice d, const VkDescriptorSetLayoutCreateInfo* ci,
                                                           const VkAllocationCallbacks* a, VkDescriptorSetLayout* out) {
  VkResult r = vk.vkCreateDescriptorSetLayout(d, ci, a, out);
  if (r == VK_SUCCESS) {
    auto l = std::make_shared<Layout>();
    const VkDescriptorSetLayoutCreateFlags uab = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    // Binding flags (pNext), push descriptors and immutable samplers: no copies.
    l->supported = !ci->pNext && !(ci->flags & ~uab);
    l->poolFlags = (ci->flags & uab) ? VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT : 0;
    for (uint32_t i = 0; i < ci->bindingCount; ++i) {
      VkDescriptorSetLayoutBinding b = ci->pBindings[i];
      if (KindOf(b.descriptorType) == Kind::Other || (b.pImmutableSamplers && HoldsSampler(b.descriptorType)))
        l->supported = false;
      b.pImmutableSamplers = nullptr;
      l->bindings.push_back(b);
    }
    std::sort(l->bindings.begin(), l->bindings.end(),
              [](const VkDescriptorSetLayoutBinding& x, const VkDescriptorSetLayoutBinding& y) { return x.binding < y.binding; });
    std::unique_lock lk(g_mx);
    g_layouts[*out] = std::move(l);
  }
  return r;
}
VKAPI_ATTR void VKAPI_CALL w_DestroyDescriptorSetLayout(VkDevice d, VkDescriptorSetLayout layout,
                                                        const VkAllocationCallbacks* a) {
  {
    std::unique_lock lk(g_mx);
    auto it = g_layouts.find(layout);
    if (it != g_layouts.end()) {
      it->second->alive = false;  // no more copies; existing ones stay valid
      g_layouts.erase(it);
    }
  }
  vk.vkDestroyDescriptorSetLayout(d, layout, a);
}
VKAPI_ATTR VkResult VKAPI_CALL w_AllocateDescriptorSets(VkDevice d, const VkDescriptorSetAllocateInfo* ai,
                                                        VkDescriptorSet* out) {
  VkResult r = vk.vkAllocateDescriptorSets(d, ai, out);
  if (r != VK_SUCCESS) return r;
  std::unique_lock lk(g_mx);
  for (uint32_t i = 0; i < ai->descriptorSetCount; ++i) {
    Forget(out[i]);
    auto l = g_layouts.find(ai->pSetLayouts[i]);
    if (l == g_layouts.end() || !l->second->supported || ai->pNext) continue;  // variable counts: no copies
    Set s;
    s.layout = l->second;
    s.handle = ai->pSetLayouts[i];
    s.pool = ai->descriptorPool;
    for (const auto& b : s.layout->bindings) s.slots.emplace_back(b.descriptorCount);
    g_sets[out[i]] = std::move(s);
    g_poolSets[ai->descriptorPool].push_back(out[i]);
  }
  return r;
}
VKAPI_ATTR VkResult VKAPI_CALL w_FreeDescriptorSets(VkDevice d, VkDescriptorPool pool, uint32_t n,
                                                    const VkDescriptorSet* sets) {
  {
    std::unique_lock lk(g_mx);
    auto& owned = g_poolSets[pool];
    for (uint32_t i = 0; i < n; ++i) {
      Forget(sets[i]);
      owned.erase(std::remove(owned.begin(), owned.end(), sets[i]), owned.end());
    }
  }
  return vk.vkFreeDescriptorSets(d, pool, n, sets);
}
void ForgetPool(VkDescriptorPool pool, bool destroyed) {
  std::unique_lock lk(g_mx);
  auto it = g_poolSets.find(pool);
  if (it == g_poolSets.end()) return;
  for (VkDescriptorSet set : it->second) Forget(set);
  if (destroyed) g_poolSets.erase(it);
  else it->second.clear();
}
VKAPI_ATTR VkResult VKAPI_CALL w_ResetDescriptorPool(VkDevice d, VkDescriptorPool pool, VkDescriptorPoolResetFlags f) {
  ForgetPool(pool, false);
  return vk.vkResetDescriptorPool(d, pool, f);
}
VKAPI_ATTR void VKAPI_CALL w_DestroyDescriptorPool(VkDevice d, VkDescriptorPool pool, const VkAllocationCallbacks* a) {
  ForgetPool(pool, true);
  vk.vkDestroyDescriptorPool(d, pool, a);
}
}  // namespace

PFN_vkVoidFunction MipBiasWrapper(const char* name) {
  struct Entry {
    const char* name;
    PFN_vkVoidFunction fn;
  };
  static const Entry table[] = {
      {"vkCreateSampler", (PFN_vkVoidFunction)w_CreateSampler},
      {"vkDestroySampler", (PFN_vkVoidFunction)w_DestroySampler},
      {"vkCreateDescriptorSetLayout", (PFN_vkVoidFunction)w_CreateDescriptorSetLayout},
      {"vkDestroyDescriptorSetLayout", (PFN_vkVoidFunction)w_DestroyDescriptorSetLayout},
      {"vkAllocateDescriptorSets", (PFN_vkVoidFunction)w_AllocateDescriptorSets},
      {"vkFreeDescriptorSets", (PFN_vkVoidFunction)w_FreeDescriptorSets},
      {"vkResetDescriptorPool", (PFN_vkVoidFunction)w_ResetDescriptorPool},
      {"vkDestroyDescriptorPool", (PFN_vkVoidFunction)w_DestroyDescriptorPool},
  };
  for (const Entry& e : table)
    if (!strcmp(e.name, name)) return e.fn;
  return nullptr;
}

void MipBiasOnUpdate(uint32_t nw, const VkWriteDescriptorSet* w, uint32_t nc, const VkCopyDescriptorSet* c) {
  std::unique_lock lk(g_mx);
  for (uint32_t i = 0; i < nw; ++i) {
    const VkWriteDescriptorSet& wr = w[i];
    auto it = g_sets.find(wr.dstSet);
    if (it == g_sets.end()) continue;
    Set& s = it->second;
    RetireCopies(s);
    const Layout& l = *s.layout;
    bool ok = !wr.pNext && Walk(l, wr.dstBinding, wr.dstArrayElement, wr.descriptorCount, [&](size_t b, uint32_t e, uint32_t k) {
      Slot& slot = s.slots[b][e];
      VkDescriptorType type = l.bindings[b].descriptorType;
      slot.written = true;
      switch (KindOf(type)) {
        case Kind::Image:
          slot.image = wr.pImageInfo[k];
          slot.bias = BiasedSlot(type, slot.image);
          break;
        case Kind::Buffer: slot.buffer = wr.pBufferInfo[k]; break;
        case Kind::Texel: slot.texel = wr.pTexelBufferView[k]; break;
        case Kind::Other: slot.written = false; break;
      }
    });
    if (!ok) s.supported = false;  // inline data or an out-of-range write: leave this set alone
    Recount(s);
  }
  for (uint32_t i = 0; i < nc; ++i) {
    const VkCopyDescriptorSet& cp = c[i];
    auto dst = g_sets.find(cp.dstSet);
    if (dst == g_sets.end()) continue;
    Set& d = dst->second;
    RetireCopies(d);
    auto src = g_sets.find(cp.srcSet);
    std::vector<Slot> moved;
    bool ok = src != g_sets.end() && src->second.supported &&
              Walk(*src->second.layout, cp.srcBinding, cp.srcArrayElement, cp.descriptorCount,
                   [&](size_t b, uint32_t e, uint32_t) { moved.push_back(src->second.slots[b][e]); }) &&
              Walk(*d.layout, cp.dstBinding, cp.dstArrayElement, cp.descriptorCount,
                   [&](size_t b, uint32_t e, uint32_t k) { d.slots[b][e] = moved[k]; });
    if (!ok) d.supported = false;  // copied from a set we do not know
    Recount(d);
  }
}

VkDescriptorSet MipBiasSet(VkDescriptorSet set) {
  int steps = g_steps.load(std::memory_order_relaxed);
  if (!steps) return set;
  {
    std::shared_lock lk(g_mx);
    auto it = g_sets.find(set);
    if (it == g_sets.end() || !it->second.biased || !it->second.supported) return set;
    for (const Copy& c : it->second.copies)
      if (c.steps == steps) return c.set;
  }
  std::unique_lock lk(g_mx);
  auto it = g_sets.find(set);
  if (it == g_sets.end() || !it->second.biased || !it->second.supported || !it->second.layout->alive) return set;
  Set& s = it->second;
  for (const Copy& c : s.copies)
    if (c.steps == steps) return c.set;
  Copy c;
  c.steps = steps;
  if (!MakeCopy(s, &c)) {
    s.supported = false;  // do not retry on every bind
    return set;
  }
  s.copies.push_back(c);
  return c.set;
}

void MipBiasOnPresent(float renderToOutput, uint64_t frame) {
  int steps = renderToOutput > 0.0f && renderToOutput < 1.0f ? (int)std::lround(std::log2(renderToOutput) * kSteps) : 0;
  if (g_steps.exchange(steps) != steps) {
    if (steps) Log("texture mip bias %+.2f while DLSS upscales (render/output width %.3f)", (float)steps / kSteps, renderToOutput);
    else Log("texture mip bias off");
  }
  std::unique_lock lk(g_mx);
  g_frame = frame;
  for (size_t i = 0; i < g_retired.size();) {
    const Retired& r = g_retired[i];
    if (r.frame > frame) {
      ++i;
      continue;
    }
    if (r.set) vk.vkFreeDescriptorSets(gDevice, r.pool, 1, &r.set);
    if (r.sampler) vk.vkDestroySampler(gDevice, r.sampler, nullptr);
    g_retired[i] = g_retired.back();
    g_retired.pop_back();
  }
}
}  // namespace feverscaler
