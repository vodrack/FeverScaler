#pragma once
#include <vulkan/vulkan.h>

#include <cstdint>

namespace feverscaler {
// DllMain: hooks LoadLibraryExW so the Vulkan hooks go in the moment SDL loads vulkan-1.dll.
void InstallLoaderHooks();

struct VkNext {
#define FEVERSCALER_FN(name) PFN_##name name = nullptr;
#include "vkfuncs.inl"
#undef FEVERSCALER_FN
  PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties = nullptr;
  PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties = nullptr;
  PFN_vkGetPhysicalDeviceFormatProperties vkGetPhysicalDeviceFormatProperties = nullptr;
};
extern VkNext vk;  // valid once gDevice is set
extern VkInstance gInstance;
extern uint32_t gApiVersion;  // the game's VkApplicationInfo::apiVersion
extern VkPhysicalDevice gPhysicalDevice;
extern VkDevice gDevice;
extern uint32_t gQueueFamily;  // family of the game's (single) queue

// Wrapper lookup, implemented by the tracker/frame modules: returns our wrapper for a device
// function name (and records `next` as the function it must forward to), or null.
PFN_vkVoidFunction LookupDeviceWrapper(const char* name, PFN_vkVoidFunction next);

// Any Vulkan function as the game would reach it without our wrappers (device-level through the
// interposer, else instance-level); for code that needs its own loader (the ImGui backend).
PFN_vkVoidFunction RawVkProc(const char* name);

// Scope guard: calls made inside it go straight to the real loader (see hooks.cpp).
struct Reentry {
  Reentry();
  ~Reentry();
};
}  // namespace feverscaler
