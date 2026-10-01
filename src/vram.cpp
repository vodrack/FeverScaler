#include "vram.h"

#include <windows.h>
#include <dxgi1_4.h>

#include <cstring>

#include "hooks.h"
#include "log.h"

namespace feverscaler {
static IDXGIAdapter3* g_adapter = nullptr;
static bool g_tried = false;

// The DXGI adapter of the GPU the game renders with (matched by LUID).
static void FindAdapter() {
  g_tried = true;
  auto gp2 = (PFN_vkGetPhysicalDeviceProperties2)RawVkProc("vkGetPhysicalDeviceProperties2");
  if (!gp2 || !gPhysicalDevice) return;
  VkPhysicalDeviceIDProperties idp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
  VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
  p2.pNext = &idp;
  gp2(gPhysicalDevice, &p2);
  if (!idp.deviceLUIDValid) return;
  LUID luid;
  memcpy(&luid, idp.deviceLUID, sizeof(luid));
  IDXGIFactory4* f = nullptr;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory4), (void**)&f))) return;
  if (FAILED(f->EnumAdapterByLuid(luid, __uuidof(IDXGIAdapter3), (void**)&g_adapter))) g_adapter = nullptr;
  f->Release();
  Log("VRAM telemetry: %s", g_adapter ? "DXGI adapter found" : "no DXGI adapter for the Vulkan device");
}

VramInfo QueryVram() {
  if (!g_tried) FindAdapter();
  VramInfo v;
  DXGI_QUERY_VIDEO_MEMORY_INFO i{};
  if (g_adapter && SUCCEEDED(g_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &i))) {
    v.valid = true;
    v.budget = i.Budget;
    v.usage = i.CurrentUsage;
  }
  return v;
}
}  // namespace feverscaler
