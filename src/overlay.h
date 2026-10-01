#pragma once
#include <vulkan/vulkan.h>

#include <cstdint>

namespace feverscaler {
// In-game dev menu (Dear ImGui), drawn into the image the game is about to present.
void OverlayToggle();
// Runs fn on the game window's thread (posted message, handled by our window subclass).
bool OverlayRunOnWindowThread(void (*fn)());
unsigned long OverlayWindowThread();
// Swapchain lifecycle, from the vkCreateSwapchainKHR / vkGetSwapchainImagesKHR wrappers.
void OverlayReleaseSwapchain();
void OverlayOnSwapchainCreated(VkSwapchainKHR sc, VkFormat format, VkExtent2D extent);
void OverlayOnSwapchainImages(VkSwapchainKHR sc, const VkImage* imgs, uint32_t n);
// Present thread, before the Streamline present. Does nothing while the menu is hidden;
// otherwise draws the menu and makes `present` wait on that work instead of the game's semaphores.
void OverlayPresent(VkQueue queue, VkPresentInfoKHR* present);
}  // namespace feverscaler
