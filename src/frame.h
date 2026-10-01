#pragma once
#include <vulkan/vulkan.h>

#include <cstdint>
#include "config.h"

namespace feverscaler {
struct CbState;
// Frame-level wrappers (submit / present / swapchain); null if the name is not one of ours.
PFN_vkVoidFunction FrameWrapper(const char* name);
// Called from the vkCmdBeginRenderPass wrapper when the UI pass (swapchain target) begins in a
// command buffer that has a HUD-less source: records the copy into our HUD-less image.
void OnUiPassBegin(VkCommandBuffer cb, CbState* st);
// Frame counter used to pick the frame slot while recording.
uint32_t RecordingSlot();

// ---- frame-generation control (present thread: the dev menu and the settings bridge) ----------
struct FgStatus {
  bool available = false;           // DLSS-G can run on this system
  const char* unavailableReason = "";  // why not, for the user
  bool userEnabled = true;          // the user's on/off switch
  bool active = false;              // DLSS-G is generating right now
  bool have3D = false;              // the last frame had valid inputs (3D view, not a menu)
  uint32_t generatedFrames = 1;     // effective fixed multiplier: 1 = x2 ... 5 = x6
  uint32_t maxGeneratedFrames = 1;  // reported capability, capped at x6
  float baseFps = 0;                // frames the game renders per second
};
FgStatus FgGetStatus();
void FgSetUserEnabled(bool on);             // also saved to the ini
void FgSetGeneratedFrames(uint32_t frames);  // also saved to the ini

// ---- DLSS Super Resolution (present thread: the dev menu) ----------------------------------------
struct SrStatus {
  bool available = false;  // sl.dlss loaded and usable on this GPU
  const char* unavailableReason = "";  // why not, for the user
  bool wanted = false;     // the user's switch
  bool active = false;     // DLSS produced the last frame's image
  const char* mode = "off";
  uint32_t renderW = 0, renderH = 0, outW = 0, outH = 0;
  uint32_t preset = 0;  // sl::DLSSPreset in use
  uint32_t renderMode = 0;    // render resolution preset (config DlssMode)
  bool scaleControl = false;  // the plugin can set the render resolution on this game build
  float sliderScale = 0;      // the game's own Resolution Scale
};
SrStatus SrGetStatus();
bool SrActive();  // DLSS upscaling this frame (any thread)
bool SrRequested();  // usable SR request, including its bounded startup attempt
void SrSetEnabled(bool on);  // also saved to the ini; takes effect at the next frame
void SrSetPreset(uint32_t preset);  // DLSS model; also saved to the ini
void SrSetMode(uint32_t mode);      // render resolution preset; also saved to the ini
}  // namespace feverscaler
