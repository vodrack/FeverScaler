#pragma once
#include <vulkan/vulkan.h>

#include <cstdint>

#include <sl.h>

namespace feverscaler {
// Loads sl.interposer.dll from <plugin dir>\feverscaler\Streamline and resolves an export from it.
void* SlProxyFn(const char* name);
// slInit (called from inside the game's vkCreateInstance call, never under the loader lock).
bool SlInit();
// After the interposer created the device: feature functions + Reflex options.
void SlOnDeviceCreated(VkPhysicalDevice physicalDevice);
// Streamline runs on the game's device: frame tokens, tags and constants work.
bool SlCoreReady();
// DLSS Frame Generation (with Reflex and PCL) can run on the game's device.
bool SlFgReady();
// Why DLSS Super Resolution / DLSS Frame Generation cannot run here, for the log, the dev menu and
// the settings page; "" while it can.
const char* SlSrUnavailableReason();
const char* SlFgUnavailableReason();

// ---- frame token / PCL marker ladder (one token per frame) -------------------------------
// Frame N's token is opened after frame N-1's present (SimulationStart), SimulationEnd +
// RenderSubmitStart go with the first submit of the frame, RenderSubmitEnd + PresentStart just
// before the interposer's present, PresentEnd right after it.
void SlOnFirstSubmit();
void SlOnPresentBegin();
void SlOnPresentEnd();
const sl::FrameToken* SlCurrentToken();

// ---- inputs -----------------------------------------------------------------------------
struct SlImage {
  VkImage image = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_UNDEFINED;
  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkImageUsageFlags usage = 0;
  uint32_t width = 0, height = 0;
};
// Tags depth + motion vectors (+ HUD-less if given) for the current frame, eValidUntilPresent.
bool SlTagFrame(const SlImage& depth, const SlImage& mvec, const SlImage* hudless);
// Removes all tags for the current frame (menus, loading screens).
void SlClearTags();
bool SlSetConstants(const sl::Constants& c);

// ---- DLSS Super Resolution / DLAA (render thread, while recording the game's command buffer) --
bool SlDlssAvailable();
// Chooses the DLSS mode whose render-size range contains render (DLAA when render == output)
// and sets the options when anything changed. False if no mode fits.
bool SlDlssConfigure(uint32_t renderW, uint32_t renderH, uint32_t outW, uint32_t outH, uint32_t preset);
const char* SlDlssModeName();
// Records DLSS into cmd for the current frame token; resources are passed with the evaluate call.
bool SlDlssEvaluate(VkCommandBuffer cmd, const SlImage& in, const SlImage& out, const SlImage& depth, const SlImage& mvec);

// ---- DLSS-G control (present thread) ------------------------------------------------------
// release: free DLSS-G's memory when off (the user switched it off) instead of keeping it for a
// quick restart (menus / loading screens).
bool SlSetGeneration(bool on, uint32_t frames, bool release = false);
uint32_t SlFramesMax();
void SlPollState();
// Live DLSS-G state for the dev menu: status bits (0 = ok), and frames shown per rendered frame
// (smoothed over roughly the last second).
struct SlFgState {
  uint32_t status = 0;
  uint32_t framesMax = 0;
  float ratio = 1.0f;
  int optionsError = 0;            // last failed enable/update; cleared on successful enable
  uint64_t fgVram = 0, srVram = 0;  // Streamline's estimates for DLSS-G and DLSS
  bool overBudget = false;          // Streamline reported "out of VRAM" in the last few seconds
};
SlFgState SlGetFgState();
}  // namespace feverscaler
