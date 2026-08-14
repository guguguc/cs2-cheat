#pragma once

// Inline hooks on the Vulkan loader's dispatch stubs, installed after the
// game is already running (injected shared object). Renders the ImGui overlay
// into the swapchain image right before the game presents it.
namespace vk_hook {

// dlopen libvulkan and patch vkQueuePresentKHR / vkCreateDevice /
// vkCreateSwapchainKHR / vkCreateInstance.
void install();
void uninstall();

// True once the hooks are in place.
bool installed();

}  // namespace vk_hook
