#include "vk_hook.h"

#include "input_x11.h"
#include "logger.h"
#include "overlay_ctx.h"
#include "overlay_draw.h"

#include "imgui.h"

#include <dlfcn.h>
#include <sys/uio.h>
#include <unistd.h>
#include <fstream>
#include <string>
#include <vector>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <thread>

VulkanHook g_vk_hook;

// --- extern "C" detours ------------------------------------------------------
// The trampoline targets need plain C functions with the exact Vulkan
// signatures. Each one just hands control to the singleton `g_vk_hook`.

extern "C" VkResult detour_create_instance(const VkInstanceCreateInfo* ci,
                                           const VkAllocationCallbacks* ac, VkInstance* out) {
    return g_vk_hook.on_create_instance(ci, ac, out);
}

extern "C" VkResult detour_create_device(VkPhysicalDevice pd, const VkDeviceCreateInfo* ci,
                                         const VkAllocationCallbacks* ac, VkDevice* out) {
    return g_vk_hook.on_create_device(pd, ci, ac, out);
}

extern "C" VkResult detour_create_swapchain(VkDevice dev, const VkSwapchainCreateInfoKHR* ci,
                                            const VkAllocationCallbacks* ac,
                                            VkSwapchainKHR* out) {
    return g_vk_hook.on_create_swapchain(dev, ci, ac, out);
}

extern "C" VkResult detour_create_swapchain_game(VkDevice dev,
                                                 const VkSwapchainCreateInfoKHR* ci,
                                                 const VkAllocationCallbacks* ac,
                                                 VkSwapchainKHR* out) {
    return g_vk_hook.on_create_swapchain_game(dev, ci, ac, out);
}

extern "C" VkResult detour_present(VkQueue queue, const VkPresentInfoKHR* pi) {
    return g_vk_hook.on_present(queue, pi);
}

// --- class implementation ----------------------------------------------------

bool VulkanHook::pointer_in_own_so(std::uintptr_t addr) const {
    std::ifstream maps("/proc/self/maps");
    std::string line;
    while (std::getline(maps, line)) {
        if (line.find("libcs2_internal.so") == std::string::npos) continue;
        const auto dash = line.find('-');
        if (dash == std::string::npos) continue;
        const std::uintptr_t start = std::stoull(line.substr(0, dash), nullptr, 16);
        const auto sp = line.find(' ', dash);
        const std::uintptr_t end = std::stoull(line.substr(dash + 1, sp - dash - 1), nullptr, 16);
        if (addr >= start && addr < end) return true;
    }
    return false;
}

// The game resolves device functions through vkGetDeviceProcAddr and calls
// them directly (the loader's own stubs are bypassed once the pointers are
// cached). Instead of chasing copies of those pointers, we inline-hook the
// function CODE itself: every call site — however many copies exist — ends up
// in our detour, and the trampoline calls the original implementation.
void VulkanHook::install_icd_hook() {
    if (!real_igpa_ || !real_gdpa_) return;

    VkInstance inst = VK_NULL_HANDLE;
    VkApplicationInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &ai;
    const auto create_instance =
        reinterpret_cast<PFN_vkCreateInstance>(real_igpa_(nullptr, "vkCreateInstance"));
    if (!create_instance || create_instance(&ici, nullptr, &inst) != VK_SUCCESS) {
        Logger::instance().error("vk_hook: icd-hook: could not create helper instance\n");
        return;
    }
    const auto enum_phys = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
        real_igpa_(inst, "vkEnumeratePhysicalDevices"));
    std::uint32_t count = 0;
    if (!enum_phys || enum_phys(inst, &count, nullptr) != VK_SUCCESS || count == 0) {
        Logger::instance().error("vk_hook: icd-hook: no physical devices\n");
        if (const auto di = reinterpret_cast<PFN_vkDestroyInstance>(
                real_igpa_(inst, "vkDestroyInstance")); di)
            di(inst, nullptr);
        return;
    }
    VkPhysicalDevice phys[4];
    if (count > 4) count = 4;
    enum_phys(inst, &count, phys);

    const char* dexts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    const float priority = 1.f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = 0;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = dexts;
    const auto create_device =
        reinterpret_cast<PFN_vkCreateDevice>(real_igpa_(inst, "vkCreateDevice"));
    VkDevice dev = VK_NULL_HANDLE;
    if (!create_device || create_device(phys[0], &dci, nullptr, &dev) != VK_SUCCESS) {
        Logger::instance().error("vk_hook: icd-hook: could not create helper device\n");
        if (const auto di = reinterpret_cast<PFN_vkDestroyInstance>(
                real_igpa_(inst, "vkDestroyInstance")); di)
            di(inst, nullptr);
        return;
    }

    // Resolve the REAL functions the game calls. With the vkGetDeviceProcAddr
    // hook removed, `real_gdpa` is the untouched loader function and returns
    // the real implementations (ICD, or the outermost layer if one is active).
    IcdPresentFn present = reinterpret_cast<IcdPresentFn>(real_gdpa_(dev, "vkQueuePresentKHR"));
    if (!present || pointer_in_own_so(reinterpret_cast<std::uintptr_t>(present))) {
        Logger::instance().error("vk_hook: icd-hook: resolved present unusable (%p) - overlay disabled\n",
                                 reinterpret_cast<void*>(present));
    } else if (hook64::install(h_icd_present_, reinterpret_cast<void*>(present),
                               reinterpret_cast<void*>(detour_present))) {
        icd_present_ = present;
        Logger::instance().log("vk_hook: inline-hooked real vkQueuePresentKHR at %p\n",
                               reinterpret_cast<void*>(present));
    } else {
        icd_present_ = present;
        Logger::instance().error("vk_hook: inline-hook failed for real vkQueuePresentKHR (%p); calling it "
                                 "directly\n", reinterpret_cast<void*>(present));
    }

    CreateSwapchainFn swapchain =
        reinterpret_cast<CreateSwapchainFn>(real_gdpa_(dev, "vkCreateSwapchainKHR"));
    if (swapchain && !pointer_in_own_so(reinterpret_cast<std::uintptr_t>(swapchain))) {
        if (hook64::install(h_icd_swapchain_, reinterpret_cast<void*>(swapchain),
                            reinterpret_cast<void*>(detour_create_swapchain_game)))
            Logger::instance().log("vk_hook: inline-hooked real vkCreateSwapchainKHR at %p\n",
                                   reinterpret_cast<void*>(swapchain));
    }

    if (const auto dd = reinterpret_cast<PFN_vkDestroyDevice>(real_igpa_(inst, "vkDestroyDevice")); dd)
        dd(dev, nullptr);

    // Destroy the helper DEVICE but keep the helper INSTANCE + physical device
    // alive (instance_ / phys_): ImGui_ImplVulkan_Init and imgui_loader use
    // them, and a destroyed instance handle segfaults the driver. The game's
    // real device is captured later by on_create_swapchain_game.
    device_ = VK_NULL_HANDLE;
    swapchain_ = VK_NULL_HANDLE;
    format_ = VK_FORMAT_UNDEFINED;
    ready_ = false;
}

VkResult VulkanHook::on_create_instance(const VkInstanceCreateInfo* ci,
                                        const VkAllocationCallbacks* ac, VkInstance* out) {
    const VkResult r = reinterpret_cast<CreateInstanceFn>(h_create_instance_.trampoline)(ci, ac, out);
    if (r == VK_SUCCESS) instance_ = *out;
    return r;
}

VkResult VulkanHook::on_create_device(VkPhysicalDevice pd, const VkDeviceCreateInfo* ci,
                                      const VkAllocationCallbacks* ac, VkDevice* out) {
    const VkResult r =
        reinterpret_cast<CreateDeviceFn>(h_create_device_.trampoline)(pd, ci, ac, out);
    if (r == VK_SUCCESS) {
        phys_ = pd;
        device_ = *out;
        if (ci->queueCreateInfoCount > 0)
            queue_family_ = ci->pQueueCreateInfos[0].queueFamilyIndex;
    }
    return r;
}

VkResult VulkanHook::on_create_swapchain(VkDevice dev, const VkSwapchainCreateInfoKHR* ci,
                                         const VkAllocationCallbacks* ac,
                                         VkSwapchainKHR* out) {
    const VkResult r =
        reinterpret_cast<CreateSwapchainFn>(h_create_swapchain_.trampoline)(dev, ci, ac, out);
    if (r == VK_SUCCESS) {
        swapchain_ = *out;
        format_ = ci->imageFormat;
        extent_ = ci->imageExtent;
        image_count_ = ci->minImageCount;
        ready_ = false;
    }
    return r;
}

// Inline-hooked copy of the REAL vkCreateSwapchainKHR the game calls. Captures
// the swapchain format/device/extent when the game (re)creates its swapchain,
// which lets the overlay initialize even if it was injected after the current
// swapchain already existed.
VkResult VulkanHook::on_create_swapchain_game(VkDevice dev,
                                              const VkSwapchainCreateInfoKHR* ci,
                                              const VkAllocationCallbacks* ac,
                                              VkSwapchainKHR* out) {
    const VkResult r =
        reinterpret_cast<CreateSwapchainFn>(h_icd_swapchain_.trampoline)(dev, ci, ac, out);
    if (r == VK_SUCCESS) {
        device_ = dev;
        swapchain_ = *out;
        format_ = ci->imageFormat;
        extent_ = ci->imageExtent;
        image_count_ = ci->minImageCount;
        ready_ = false;
        Logger::instance().log("vk_hook: swapchain recreated (game): %p format=%d %ux%u\n",
                               static_cast<void*>(*out), static_cast<int>(ci->imageFormat),
                               extent_.width, extent_.height);
        // The game is NOT presenting while it creates the swapchain, so this
        // is the safe moment to build the overlay's Vulkan objects (pipeline
        // creation deadlocks the NVIDIA driver if it races vkQueuePresentKHR).
        if (last_queue_ && !ready_) {
            if (init_render(last_queue_))
                Logger::instance().log("vk_hook: overlay render initialized\n");
            else
                Logger::instance().error("vk_hook: init from swapchain creation FAILED\n");
        }
    }
    return r;
}

VkResult VulkanHook::on_present(VkQueue queue, const VkPresentInfoKHR* pi) {
    // The ORIGINAL present implementation. When the inline hook is active,
    // icd_present_ points at the patched function (its code starts with a
    // jump to us), so we must go through the trampoline to avoid recursion.
    auto real_present = [&](VkQueue q, const VkPresentInfoKHR* p) -> VkResult {
        if (h_icd_present_.trampoline)
            return reinterpret_cast<PresentFn>(h_icd_present_.trampoline)(q, p);
        if (icd_present_ && icd_present_ != reinterpret_cast<IcdPresentFn>(detour_present))
            return icd_present_(q, p);
        if (h_present_.trampoline)
            return reinterpret_cast<PresentFn>(h_present_.trampoline)(q, p);
        return VK_ERROR_DEVICE_LOST;
    };

    // Reentrancy guard: if the real present (e.g. the Steam overlay layer's
    // wrapper) routes back through one of our patched pointers, do not render
    // again — just pass through.
    static thread_local bool in_detour = false;
    if (in_detour || !pi || pi->swapchainCount < 1) {
        return real_present(queue, pi);
    }
    in_detour = true;
    last_queue_ = queue;

    if (hooked_) {
        const VkSwapchainKHR sc = pi->pSwapchains[0];
        const std::uint32_t idx = pi->pImageIndices[0];
        if (sc != swapchain_) ready_ = false;  // swapchain recreated
        // NOTE: the overlay's Vulkan objects are built inside
        // on_create_swapchain_game (the game is not presenting then).
        // Creating pipelines here would deadlock the NVIDIA driver.
        // Diagnostic opt-out: `touch /tmp/cs2_no_overlay` skips the ImGui
        // overlay (isolates whether present-hook rendering affects the game).
        static bool no_overlay = false;
        static auto last_chk = std::chrono::steady_clock::now();
        if (std::chrono::steady_clock::now() - last_chk > std::chrono::seconds(1)) {
            last_chk = std::chrono::steady_clock::now();
            no_overlay = static_cast<bool>(std::ifstream("/tmp/cs2_no_overlay"));
        }
        if (ready_ && !no_overlay) render_frame(queue, idx);
        else if (!format_warned_ && format_ == VK_FORMAT_UNDEFINED) {
            format_warned_ = true;
            Logger::instance().error("vk_hook: swapchain format unknown - the overlay was likely injected "
                                     "after the game created its swapchain. Recreate the swapchain "
                                     "(join a match / toggle fullscreen) or inject earlier.\n");
        }
    }
    const VkResult result = real_present(queue, pi);
    in_detour = false;
    return result;
}

PFN_vkVoidFunction VulkanHook::imgui_loader(const char* name, void*) {
    if (g_vk_hook.real_igpa_ && g_vk_hook.instance_)
        if (PFN_vkVoidFunction p = g_vk_hook.real_igpa_(g_vk_hook.instance_, name)) return p;
    if (g_vk_hook.real_gdpa_ && g_vk_hook.device_)
        if (PFN_vkVoidFunction p = g_vk_hook.real_gdpa_(g_vk_hook.device_, name)) return p;
    return reinterpret_cast<PFN_vkVoidFunction>(dlsym(RTLD_DEFAULT, name));
}

bool VulkanHook::init_render(VkQueue queue) {
    if (!device_ || swapchain_ == VK_NULL_HANDLE || format_ == VK_FORMAT_UNDEFINED)
        return false;
    resolve_device_functions(device_);

    // Find the queue family that owns the present queue (avoids a mismatch
    // between our command pool and the queue we submit on).
    if (fp_GetDeviceQueue) {
        for (std::uint32_t fam = 0; fam < 16; ++fam) {
            VkQueue q = VK_NULL_HANDLE;
            fp_GetDeviceQueue(device_, fam, 0, &q);
            if (q == queue) {
                queue_family_ = fam;
                break;
            }
        }
    }
    if (!fp_GetSwapchainImagesKHR || !fp_CreateRenderPass || !fp_CreateImageView ||
        !fp_CreateFramebuffer || !fp_CreateCommandPool || !fp_AllocateCommandBuffers ||
        !fp_BeginCommandBuffer || !fp_CmdPipelineBarrier || !fp_CmdBeginRenderPass ||
        !fp_CmdEndRenderPass || !fp_EndCommandBuffer || !fp_QueueSubmit || !fp_QueueWaitIdle)
        return false;

    // --- descriptor pool (for ImGui image views) ------------------------------
    const VkDescriptorPoolSize pool_sizes[] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16},
    };
    const VkDescriptorPoolCreateInfo pool_info = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, 16, 1, pool_sizes};
    if (fp_CreateDescriptorPool(device_, &pool_info, nullptr, &pool_) != VK_SUCCESS) return false;

    // --- render pass (loadOp = LOAD keeps the game's frame underneath) -------
    const VkAttachmentDescription att = {
        0, format_, VK_SAMPLE_COUNT_1_BIT,
        VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
        VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    const VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    const VkSubpassDescription subpass = {0, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                          0, nullptr, 1, &ref, nullptr, nullptr, 0, nullptr};
    const VkSubpassDependency dep = {
        VK_SUBPASS_EXTERNAL, 0,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_DEPENDENCY_BY_REGION_BIT};
    const VkRenderPassCreateInfo rp_info = {
        VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, nullptr, 0, 1, &att, 1, &subpass, 1, &dep};
    if (fp_CreateRenderPass(device_, &rp_info, nullptr, &rp_) != VK_SUCCESS) return false;

    // --- swapchain images + views + framebuffers ------------------------------
    std::uint32_t count = 0;
    if (fp_GetSwapchainImagesKHR(device_, swapchain_, &count, nullptr) != VK_SUCCESS ||
        count == 0)
        return false;
    image_count_ = count;
    images_ = new VkImage[count];
    views_ = new VkImageView[count];
    fbs_ = new VkFramebuffer[count];
    if (fp_GetSwapchainImagesKHR(device_, swapchain_, &count, images_) != VK_SUCCESS)
        return false;
    for (std::uint32_t i = 0; i < count; ++i) {
        const VkImageViewCreateInfo vi = {
            VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, nullptr, 0,
            images_[i], VK_IMAGE_VIEW_TYPE_2D, format_,
            {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
             VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
            {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        if (fp_CreateImageView(device_, &vi, nullptr, &views_[i]) != VK_SUCCESS) return false;
        const VkFramebufferCreateInfo fb = {
            VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, nullptr, 0,
            rp_, 1, &views_[i], extent_.width, extent_.height, 1};
        if (fp_CreateFramebuffer(device_, &fb, nullptr, &fbs_[i]) != VK_SUCCESS) return false;
    }

    // --- command pool + buffer ------------------------------------------------
    const VkCommandPoolCreateInfo cp = {
        VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr,
        VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, queue_family_};
    if (fp_CreateCommandPool(device_, &cp, nullptr, &cp_) != VK_SUCCESS) return false;
    const VkCommandBufferAllocateInfo cba = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, cp_,
        VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    if (fp_AllocateCommandBuffers(device_, &cba, &cmd_) != VK_SUCCESS) return false;

    // --- ImGui ----------------------------------------------------------------
    // The overlay is re-initialized every time the game recreates its
    // swapchain (resolution/fullscreen toggle). ImGui must only be created
    // ONCE - recreating the context and reloading fonts froze the game on
    // resolution switches.
    if (!renderer_.init(instance_, phys_, device_, queue_family_, queue, pool_, rp_,
                        image_count_, &VulkanHook::imgui_loader)) {
        Logger::instance().error("vk_hook: imgui renderer init failed\n");
        return false;
    }

    ready_ = true;
    return true;
}

void VulkanHook::render_frame(VkQueue queue, std::uint32_t image_index) {
    if (image_index >= image_count_) return;

    ImGuiIO& io = renderer_.io();
    io.DisplaySize = ImVec2(static_cast<float>(extent_.width),
                            static_cast<float>(extent_.height));
    io.MouseDrawCursor = g_ctx.panel_open;  // only show the cursor in the menu
    input_x11::poll(io);

    renderer_.new_frame();
    {
        std::lock_guard<std::mutex> lk(g_ctx.mtx);
        g_overlay.esp(g_ctx, io.DisplaySize);
        g_overlay.panel(g_ctx);
        g_overlay.aim_hint(g_ctx, io.DisplaySize);
    }
    fp_QueueWaitIdle(queue);

    const VkCommandBufferBeginInfo bi = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
        VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};
    fp_BeginCommandBuffer(cmd_, &bi);

    const VkImageMemoryBarrier to_color = {
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr,
        0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
        images_[image_index], {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    fp_CmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr,
                          1, &to_color);

    const VkRenderPassBeginInfo rp = {
        VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, nullptr,
        rp_, fbs_[image_index],
        {{0, 0}, {extent_.width, extent_.height}},
        0, nullptr};
    fp_CmdBeginRenderPass(cmd_, &rp, VK_SUBPASS_CONTENTS_INLINE);
    renderer_.render_frame(cmd_);
    fp_CmdEndRenderPass(cmd_);

    const VkImageMemoryBarrier to_present = {
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
        images_[image_index], {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    fp_CmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1,
                          &to_present);
    fp_EndCommandBuffer(cmd_);

    const VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 0, nullptr, nullptr, 1,
                             &cmd_, 0, nullptr};
    fp_QueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    fp_QueueWaitIdle(queue);
}

void VulkanHook::resolve_device_functions(VkDevice device) {
    if (!real_gdpa_) return;
#define VKF(name) fp_##name = reinterpret_cast<PFN_vk##name>(real_gdpa_(device, "vk" #name))
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
}

void VulkanHook::install() {
    if (hooked_) return;
    void* vk = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!vk) {
        Logger::instance().error("vk_hook: dlopen libvulkan failed\n");
        return;
    }
    real_igpa_ = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(vk, "vkGetInstanceProcAddr"));
    real_gdpa_ = reinterpret_cast<PFN_vkGetDeviceProcAddr>(dlsym(vk, "vkGetDeviceProcAddr"));
    hook64::install(h_create_instance_, dlsym(vk, "vkCreateInstance"),
                    reinterpret_cast<void*>(detour_create_instance));
    hook64::install(h_create_device_, dlsym(vk, "vkCreateDevice"),
                    reinterpret_cast<void*>(detour_create_device));
    hooked_ = true;
    Logger::instance().log("vk_hook: loader resolved, installing ICD hooks\n");
    install_icd_hook();
}

void VulkanHook::uninstall() {
    hook64::uninstall(h_icd_present_);
    hook64::uninstall(h_icd_swapchain_);
    hook64::uninstall(h_create_instance_);
    hook64::uninstall(h_create_device_);
    hooked_ = false;
    Logger::instance().log("vk_hook: uninstalled\n");
}
