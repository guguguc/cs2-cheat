#pragma once

#include <cstdint>

#include "hook_x64.h"
#include "imgui_renderer.h"

#include <vulkan/vulkan.h>

// Inline hooks on the Vulkan loader's dispatch stubs, installed after the
// game is already running (injected shared object). Renders the ImGui overlay
// into the swapchain image right before the game presents it.
//
// The extern "C" detour trampolines (detour_*) are thin wrappers that hand
// control to the single `g_vk_hook` instance, which owns all Vulkan state.
class VulkanHook {
public:
    // dlopen libvulkan and patch vkQueuePresentKHR / vkCreateDevice /
    // vkCreateSwapchainKHR / vkCreateInstance.
    void install();
    void uninstall();
    // True once the hooks are in place.
    bool installed() const { return hooked_; }

    // --- extern "C" detour entry points (called by the trampolines) ---------
    VkResult on_create_instance(const VkInstanceCreateInfo* ci,
                                const VkAllocationCallbacks* ac, VkInstance* out);
    VkResult on_create_device(VkPhysicalDevice pd, const VkDeviceCreateInfo* ci,
                              const VkAllocationCallbacks* ac, VkDevice* out);
    VkResult on_create_swapchain(VkDevice dev, const VkSwapchainCreateInfoKHR* ci,
                                 const VkAllocationCallbacks* ac, VkSwapchainKHR* out);
    VkResult on_create_swapchain_game(VkDevice dev, const VkSwapchainCreateInfoKHR* ci,
                                      const VkAllocationCallbacks* ac, VkSwapchainKHR* out);
    VkResult on_present(VkQueue queue, const VkPresentInfoKHR* pi);

private:
    using IcdPresentFn = VkResult (*)(VkQueue, const VkPresentInfoKHR*);
    using GdpaFn = PFN_vkVoidFunction (*)(VkDevice, const char*);
    using PresentFn = VkResult (*)(VkQueue, const VkPresentInfoKHR*);
    using CreateDeviceFn = VkResult (*)(VkPhysicalDevice, const VkDeviceCreateInfo*,
                                        const VkAllocationCallbacks*, VkDevice*);
    using CreateSwapchainFn = VkResult (*)(VkDevice, const VkSwapchainCreateInfoKHR*,
                                           const VkAllocationCallbacks*, VkSwapchainKHR*);
    using CreateInstanceFn = VkResult (*)(const VkInstanceCreateInfo*,
                                          const VkAllocationCallbacks*, VkInstance*);

    // --- loader entry points -------------------------------------------------
    PFN_vkGetInstanceProcAddr real_igpa_ = nullptr;
    PFN_vkGetDeviceProcAddr real_gdpa_ = nullptr;

    // --- captured handles ----------------------------------------------------
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice phys_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    std::uint32_t queue_family_ = 0;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{};
    std::uint32_t image_count_ = 0;
    VkImage* images_ = nullptr;
    VkImageView* views_ = nullptr;
    VkFramebuffer* fbs_ = nullptr;
    VkRenderPass rp_ = VK_NULL_HANDLE;
    VkCommandPool cp_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    bool ready_ = false;
    bool hooked_ = false;
    bool format_warned_ = false;
    VkQueue last_queue_ = VK_NULL_HANDLE;
    IcdPresentFn icd_present_ = nullptr;

    // --- inline hooks ---------------------------------------------------------
    hook64::Hook h_present_;
    hook64::Hook h_create_device_;
    hook64::Hook h_create_swapchain_;
    hook64::Hook h_create_instance_;
    hook64::Hook h_icd_present_;    // inline hook on the real vkQueuePresentKHR
    hook64::Hook h_icd_swapchain_;  // inline hook on the real vkCreateSwapchainKHR

    // --- real device functions (resolved through the loader) -----------------
#define VKF(name) PFN_vk##name fp_##name = nullptr
    VKF(GetDeviceQueue);
    VKF(GetSwapchainImagesKHR);
    VKF(CreateRenderPass);
    VKF(DestroyRenderPass);
    VKF(CreateImageView);
    VKF(DestroyImageView);
    VKF(CreateFramebuffer);
    VKF(DestroyFramebuffer);
    VKF(CreateCommandPool);
    VKF(DestroyCommandPool);
    VKF(AllocateCommandBuffers);
    VKF(FreeCommandBuffers);
    VKF(BeginCommandBuffer);
    VKF(EndCommandBuffer);
    VKF(ResetCommandBuffer);
    VKF(CmdPipelineBarrier);
    VKF(CmdBeginRenderPass);
    VKF(CmdEndRenderPass);
    VKF(QueueSubmit);
    VKF(QueueWaitIdle);
    VKF(CreateDescriptorPool);
    VKF(DestroyDescriptorPool);
#undef VKF

    void install_icd_hook();
    bool init_render(VkQueue queue);
    void render_frame(VkQueue queue, std::uint32_t image_index);
    void resolve_device_functions(VkDevice device);
    bool pointer_in_own_so(std::uintptr_t addr) const;
    static PFN_vkVoidFunction imgui_loader(const char* name, void*);

    // --- ImGui overlay --------------------------------------------------------
    ImGuiRenderer renderer_;
};

// The single hook instance; the extern "C" detours dispatch to it.
extern VulkanHook g_vk_hook;
