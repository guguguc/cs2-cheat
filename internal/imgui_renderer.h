#pragma once

#include <cstdint>

#include <vulkan/vulkan.h>

struct ImGuiIO;

// Wraps the ImGui context + Vulkan backend: context creation, theme, and the
// per-frame lifecycle (NewFrame / Render / RenderDrawData). The Vulkan swapchain
// resources (render pass, command buffer, framebuffers) stay owned by VulkanHook;
// we only initialize the backend against them and drive ImGui.
class ImGuiRenderer {
public:
    // Loader used by ImGui_ImplVulkan_LoadFunctions (resolves vk functions
    // through the untouched loader trampolines).
    using LoaderFn = PFN_vkVoidFunction (*)(const char* name, void* user_data);

    // Creates the ImGui context, applies the theme and initializes the Vulkan
    // backend. Called on every swapchain (re)creation, exactly like before.
    bool init(VkInstance instance, VkPhysicalDevice phys, VkDevice device,
              std::uint32_t queue_family, VkQueue queue, VkDescriptorPool pool,
              VkRenderPass rp, std::uint32_t image_count, LoaderFn loader);
    // Frees the Vulkan backend (safe to skip on dlclose).
    void shutdown();

    // Access to ImGuiIO so the input poller can feed mouse events.
    ImGuiIO& io() const;

    // ImGui_ImplVulkan_NewFrame + ImGui::NewFrame (call once per frame before
    // the overlay draws).
    void new_frame() const;

    // ImGui::Render + ImGui_ImplVulkan_RenderDrawData into `cmd` (call inside
    // an active render pass).
    void render_frame(VkCommandBuffer cmd) const;

    // Applies the translucent dark theme to the current ImGui context.
    static void apply_theme();

private:
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice phys_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    std::uint32_t queue_family_ = 0;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkRenderPass rp_ = VK_NULL_HANDLE;
    std::uint32_t image_count_ = 0;
};
