#include "vk_hook.h"

#include "hook_x64.h"
#include "input_x11.h"
#include "overlay_ctx.h"
#include "overlay_draw.h"

#include <vulkan/vulkan.h>

#include "imgui.h"
#include "backends/imgui_impl_vulkan.h"

#include <dlfcn.h>
#include <sys/uio.h>
#include <unistd.h>
#include <fstream>
#include <string>
#include <vector>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <thread>

// Defined at the end of this file (global scope); referenced from inside the
// anonymous namespace below, so it needs a file-scope declaration here.
extern "C" VkResult detour_present(VkQueue, const VkPresentInfoKHR*);

namespace {

// --- loader entry points -----------------------------------------------------
PFN_vkGetInstanceProcAddr real_igpa = nullptr;
PFN_vkGetDeviceProcAddr real_gdpa = nullptr;

// --- captured handles --------------------------------------------------------
VkInstance g_instance = VK_NULL_HANDLE;
VkPhysicalDevice g_phys = VK_NULL_HANDLE;
VkDevice g_device = VK_NULL_HANDLE;
std::uint32_t g_queue_family = 0;
VkSwapchainKHR g_swapchain = VK_NULL_HANDLE;
VkFormat g_format = VK_FORMAT_UNDEFINED;
VkExtent2D g_extent{};
std::uint32_t g_image_count = 0;
VkImage* g_images = nullptr;
VkImageView* g_views = nullptr;
VkFramebuffer* g_fbs = nullptr;
VkRenderPass g_rp = VK_NULL_HANDLE;
VkCommandPool g_cp = VK_NULL_HANDLE;
VkCommandBuffer g_cmd = VK_NULL_HANDLE;
VkDescriptorPool g_pool = VK_NULL_HANDLE;
bool g_ready = false;
bool g_hooked = false;
bool g_format_warned_ = false;
VkQueue g_last_queue = VK_NULL_HANDLE;

// Real device functions the game holds, resolved through the loader's gdpa
// trampoline (bypassing our own hooks) and inline-hooked so every call goes
// through us regardless of which pointer/copy the game uses.
using IcdPresentFn = VkResult (*)(VkQueue, const VkPresentInfoKHR*);
using GdpaFn = PFN_vkVoidFunction (*)(VkDevice, const char*);
using PresentFn = VkResult (*)(VkQueue, const VkPresentInfoKHR*);
using CreateDeviceFn = VkResult (*)(VkPhysicalDevice, const VkDeviceCreateInfo*,
                                    const VkAllocationCallbacks*, VkDevice*);
using CreateSwapchainFn = VkResult (*)(VkDevice, const VkSwapchainCreateInfoKHR*,
                                       const VkAllocationCallbacks*, VkSwapchainKHR*);
using CreateInstanceFn = VkResult (*)(const VkInstanceCreateInfo*,
                                      const VkAllocationCallbacks*, VkInstance*);
IcdPresentFn g_icd_present = nullptr;

void log_(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

extern "C" VkResult detour_create_swapchain_game(VkDevice, const VkSwapchainCreateInfoKHR*,
                                                 const VkAllocationCallbacks*, VkSwapchainKHR*);
bool init_render(VkQueue queue);

hook64::Hook h_present;
hook64::Hook h_create_device;
hook64::Hook h_create_swapchain;
hook64::Hook h_create_instance;
hook64::Hook h_icd_present;    // inline hook on the real vkQueuePresentKHR
hook64::Hook h_icd_swapchain;  // inline hook on the real vkCreateSwapchainKHR

// --- real device functions (resolved through the loader) ---------------------
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

void resolve_device_functions(VkDevice device) {
    if (!real_gdpa) return;
#define VKF(name) fp_##name = reinterpret_cast<PFN_vk##name>(real_gdpa(device, "vk" #name))
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

// True when `addr` falls inside any mapping backed by our own shared object.
// Used to reject a "resolved" vkQueuePresentKHR that is actually our own
// detour (which would make the overlay recurse).
bool pointer_in_own_so(std::uintptr_t addr) {
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
void install_icd_hook() {
    if (!real_igpa || !real_gdpa) return;

    VkInstance inst = VK_NULL_HANDLE;
    VkApplicationInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &ai;
    const auto create_instance =
        reinterpret_cast<PFN_vkCreateInstance>(real_igpa(nullptr, "vkCreateInstance"));
    if (!create_instance || create_instance(&ici, nullptr, &inst) != VK_SUCCESS) {
        log_("vk_hook: icd-hook: could not create helper instance\n");
        return;
    }
    const auto enum_phys = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
        real_igpa(inst, "vkEnumeratePhysicalDevices"));
    std::uint32_t count = 0;
    if (!enum_phys || enum_phys(inst, &count, nullptr) != VK_SUCCESS || count == 0) {
        log_("vk_hook: icd-hook: no physical devices\n");
        if (const auto di = reinterpret_cast<PFN_vkDestroyInstance>(
                real_igpa(inst, "vkDestroyInstance")); di)
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
        reinterpret_cast<PFN_vkCreateDevice>(real_igpa(inst, "vkCreateDevice"));
    VkDevice dev = VK_NULL_HANDLE;
    if (!create_device || create_device(phys[0], &dci, nullptr, &dev) != VK_SUCCESS) {
        log_("vk_hook: icd-hook: could not create helper device\n");
        if (const auto di = reinterpret_cast<PFN_vkDestroyInstance>(
                real_igpa(inst, "vkDestroyInstance")); di)
            di(inst, nullptr);
        return;
    }

    // Resolve the REAL functions the game calls. With the vkGetDeviceProcAddr
    // hook removed, `real_gdpa` is the untouched loader function and returns
    // the real implementations (ICD, or the outermost layer if one is active).
    IcdPresentFn present = reinterpret_cast<IcdPresentFn>(real_gdpa(dev, "vkQueuePresentKHR"));
    if (!present || pointer_in_own_so(reinterpret_cast<std::uintptr_t>(present))) {
        log_("vk_hook: icd-hook: resolved present unusable (%p) - overlay disabled\n",
             reinterpret_cast<void*>(present));
    } else if (hook64::install(h_icd_present, reinterpret_cast<void*>(present),
                               reinterpret_cast<void*>(detour_present))) {
        g_icd_present = present;
        log_("vk_hook: inline-hooked real vkQueuePresentKHR at %p\n",
             reinterpret_cast<void*>(present));
    } else {
        g_icd_present = present;
        log_("vk_hook: inline-hook failed for real vkQueuePresentKHR (%p); calling it "
             "directly\n", reinterpret_cast<void*>(present));
    }

    CreateSwapchainFn swapchain =
        reinterpret_cast<CreateSwapchainFn>(real_gdpa(dev, "vkCreateSwapchainKHR"));
    if (swapchain && !pointer_in_own_so(reinterpret_cast<std::uintptr_t>(swapchain))) {
        if (hook64::install(h_icd_swapchain, reinterpret_cast<void*>(swapchain),
                            reinterpret_cast<void*>(detour_create_swapchain_game)))
            log_("vk_hook: inline-hooked real vkCreateSwapchainKHR at %p\n",
                 reinterpret_cast<void*>(swapchain));
    }

    if (const auto dd = reinterpret_cast<PFN_vkDestroyDevice>(real_igpa(inst, "vkDestroyDevice")); dd)
        dd(dev, nullptr);

    // Destroy the helper DEVICE but keep the helper INSTANCE + physical device
    // alive (g_instance / g_phys): ImGui_ImplVulkan_Init and imgui_loader use
    // them, and a destroyed instance handle segfaults the driver. The game's
    // real device is captured later by detour_create_swapchain_game.
    g_device = VK_NULL_HANDLE;
    g_swapchain = VK_NULL_HANDLE;
    g_format = VK_FORMAT_UNDEFINED;
    g_ready = false;
}

void log_(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void log_(const char* fmt, ...) {
    FILE* f = std::fopen("/tmp/cs2_internal.log", "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(f, fmt, ap);
    va_end(ap);
    std::fclose(f);
}

// --- detours -----------------------------------------------------------------

extern "C" VkResult detour_create_instance(const VkInstanceCreateInfo* ci,
                                           const VkAllocationCallbacks* ac, VkInstance* out) {
    const VkResult r = reinterpret_cast<CreateInstanceFn>(h_create_instance.trampoline)(ci, ac, out);
    if (r == VK_SUCCESS) g_instance = *out;
    return r;
}

extern "C" VkResult detour_create_device(VkPhysicalDevice pd, const VkDeviceCreateInfo* ci,
                                         const VkAllocationCallbacks* ac, VkDevice* out) {
    const VkResult r =
        reinterpret_cast<CreateDeviceFn>(h_create_device.trampoline)(pd, ci, ac, out);
    if (r == VK_SUCCESS) {
        g_phys = pd;
        g_device = *out;
        if (ci->queueCreateInfoCount > 0)
            g_queue_family = ci->pQueueCreateInfos[0].queueFamilyIndex;
    }
    return r;
}

extern "C" VkResult detour_create_swapchain(VkDevice dev, const VkSwapchainCreateInfoKHR* ci,
                                            const VkAllocationCallbacks* ac,
                                            VkSwapchainKHR* out) {
    const VkResult r =
        reinterpret_cast<CreateSwapchainFn>(h_create_swapchain.trampoline)(dev, ci, ac, out);
    if (r == VK_SUCCESS) {
        g_swapchain = *out;
        g_format = ci->imageFormat;
        g_extent = ci->imageExtent;
        g_image_count = ci->minImageCount;
        g_ready = false;
    }
    return r;
}

// Inline-hooked copy of the REAL vkCreateSwapchainKHR the game calls. Captures
// the swapchain format/device/extent when the game (re)creates its swapchain,
// which lets the overlay initialize even if it was injected after the current
// swapchain already existed.
extern "C" VkResult detour_create_swapchain_game(VkDevice dev,
                                                 const VkSwapchainCreateInfoKHR* ci,
                                                 const VkAllocationCallbacks* ac,
                                                 VkSwapchainKHR* out) {
    const VkResult r =
        reinterpret_cast<CreateSwapchainFn>(h_icd_swapchain.trampoline)(dev, ci, ac, out);
    if (r == VK_SUCCESS) {
        g_device = dev;
        g_swapchain = *out;
        g_format = ci->imageFormat;
        g_extent = ci->imageExtent;
        g_image_count = ci->minImageCount;
        g_ready = false;
        log_("vk_hook: swapchain recreated (game): %p format=%d %ux%u\n",
             static_cast<void*>(*out), static_cast<int>(ci->imageFormat),
             g_extent.width, g_extent.height);
        // The game is NOT presenting while it creates the swapchain, so this
        // is the safe moment to build the overlay's Vulkan objects (pipeline
        // creation deadlocks the NVIDIA driver if it races vkQueuePresentKHR).
        if (g_last_queue && !g_ready) {
            if (init_render(g_last_queue))
                log_("vk_hook: overlay render initialized\n");
            else
                log_("vk_hook: init from swapchain creation FAILED\n");
        }
    }
    return r;
}

PFN_vkVoidFunction imgui_loader(const char* name, void*) {
    if (real_igpa && g_instance)
        if (PFN_vkVoidFunction p = real_igpa(g_instance, name)) return p;
    if (real_gdpa && g_device)
        if (PFN_vkVoidFunction p = real_gdpa(g_device, name)) return p;
    return reinterpret_cast<PFN_vkVoidFunction>(dlsym(RTLD_DEFAULT, name));
}

bool init_render(VkQueue queue) {
    if (!g_device || g_swapchain == VK_NULL_HANDLE || g_format == VK_FORMAT_UNDEFINED)
        return false;
    resolve_device_functions(g_device);

    // Find the queue family that owns the present queue (avoids a mismatch
    // between our command pool and the queue we submit on).
    if (fp_GetDeviceQueue) {
        for (std::uint32_t fam = 0; fam < 16; ++fam) {
            VkQueue q = VK_NULL_HANDLE;
            fp_GetDeviceQueue(g_device, fam, 0, &q);
            if (q == queue) {
                g_queue_family = fam;
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
    if (fp_CreateDescriptorPool(g_device, &pool_info, nullptr, &g_pool) != VK_SUCCESS) return false;

    // --- render pass (loadOp = LOAD keeps the game's frame underneath) -------
    const VkAttachmentDescription att = {
        0, g_format, VK_SAMPLE_COUNT_1_BIT,
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
    if (fp_CreateRenderPass(g_device, &rp_info, nullptr, &g_rp) != VK_SUCCESS) return false;

    // --- swapchain images + views + framebuffers ------------------------------
    std::uint32_t count = 0;
    if (fp_GetSwapchainImagesKHR(g_device, g_swapchain, &count, nullptr) != VK_SUCCESS ||
        count == 0)
        return false;
    g_image_count = count;
    g_images = new VkImage[count];
    g_views = new VkImageView[count];
    g_fbs = new VkFramebuffer[count];
    if (fp_GetSwapchainImagesKHR(g_device, g_swapchain, &count, g_images) != VK_SUCCESS)
        return false;
    for (std::uint32_t i = 0; i < count; ++i) {
        const VkImageViewCreateInfo vi = {
            VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, nullptr, 0,
            g_images[i], VK_IMAGE_VIEW_TYPE_2D, g_format,
            {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
             VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
            {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        if (fp_CreateImageView(g_device, &vi, nullptr, &g_views[i]) != VK_SUCCESS) return false;
        const VkFramebufferCreateInfo fb = {
            VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, nullptr, 0,
            g_rp, 1, &g_views[i], g_extent.width, g_extent.height, 1};
        if (fp_CreateFramebuffer(g_device, &fb, nullptr, &g_fbs[i]) != VK_SUCCESS) return false;
    }

    // --- command pool + buffer ------------------------------------------------
    const VkCommandPoolCreateInfo cp = {
        VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr,
        VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, g_queue_family};
    if (fp_CreateCommandPool(g_device, &cp, nullptr, &g_cp) != VK_SUCCESS) return false;
    const VkCommandBufferAllocateInfo cba = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, g_cp,
        VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    if (fp_AllocateCommandBuffers(g_device, &cba, &g_cmd) != VK_SUCCESS) return false;

    // --- ImGui ----------------------------------------------------------------
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    // The game hides the OS cursor (FPS), so ImGui must draw its own so the
    // menu is usable.
    ImGui::GetIO().MouseDrawCursor = true;
    ImGui::GetIO().IniFilename = "/tmp/cs2_internal_imgui.ini";
    if (!ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_3, imgui_loader, nullptr)) {
        log_("vk_hook: ImGui_ImplVulkan_LoadFunctions failed\n");
        return false;
    }
    ImGui_ImplVulkan_InitInfo info = {};
    info.ApiVersion = VK_API_VERSION_1_3;
    info.Instance = g_instance;
    info.PhysicalDevice = g_phys;
    info.Device = g_device;
    info.QueueFamily = g_queue_family;
    info.Queue = queue;
    info.DescriptorPool = g_pool;
    info.MinImageCount = 2;
    info.ImageCount = g_image_count;
    info.PipelineInfoMain.RenderPass = g_rp;
    info.PipelineInfoMain.Subpass = 0;
    info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    info.UseDynamicRendering = false;
    if (!ImGui_ImplVulkan_Init(&info)) {
        log_("vk_hook: ImGui_ImplVulkan_Init failed\n");
        return false;
    }

    g_ready = true;
    return true;
}

void render_frame(VkQueue queue, std::uint32_t image_index) {
    if (image_index >= g_image_count) return;

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(g_extent.width),
                            static_cast<float>(g_extent.height));
    io.MouseDrawCursor = g_ctx.panel_open;  // only show the cursor in the menu
    input_x11::poll(io);

    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    {
        std::lock_guard<std::mutex> lk(g_ctx.mtx);
        overlay_draw::esp(g_ctx, io.DisplaySize);
        overlay_draw::panel(g_ctx);
    }
    ImGui::Render();
    fp_QueueWaitIdle(queue);

    const VkCommandBufferBeginInfo bi = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
        VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};
    fp_BeginCommandBuffer(g_cmd, &bi);

    const VkImageMemoryBarrier to_color = {
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr,
        0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
        g_images[image_index], {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    fp_CmdPipelineBarrier(g_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr,
                          1, &to_color);

    const VkRenderPassBeginInfo rp = {
        VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, nullptr,
        g_rp, g_fbs[image_index],
        {{0, 0}, {g_extent.width, g_extent.height}},
        0, nullptr};
    fp_CmdBeginRenderPass(g_cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), g_cmd);
    fp_CmdEndRenderPass(g_cmd);

    const VkImageMemoryBarrier to_present = {
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
        g_images[image_index], {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    fp_CmdPipelineBarrier(g_cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1,
                          &to_present);
    fp_EndCommandBuffer(g_cmd);

    const VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 0, nullptr, nullptr, 1,
                             &g_cmd, 0, nullptr};
    fp_QueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    fp_QueueWaitIdle(queue);
}

}  // namespace

extern "C" VkResult detour_present(VkQueue queue, const VkPresentInfoKHR* pi) {
    // The ORIGINAL present implementation. When the inline hook is active,
    // g_icd_present points at the patched function (its code starts with a
    // jump to us), so we must go through the trampoline to avoid recursion.
    auto real_present = [&](VkQueue q, const VkPresentInfoKHR* p) -> VkResult {
        if (h_icd_present.trampoline)
            return reinterpret_cast<PresentFn>(h_icd_present.trampoline)(q, p);
        if (g_icd_present && g_icd_present != reinterpret_cast<IcdPresentFn>(detour_present))
            return g_icd_present(q, p);
        if (h_present.trampoline)
            return reinterpret_cast<PresentFn>(h_present.trampoline)(q, p);
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
    g_last_queue = queue;

    if (g_hooked) {
        const VkSwapchainKHR sc = pi->pSwapchains[0];
        const std::uint32_t idx = pi->pImageIndices[0];
        if (sc != g_swapchain) g_ready = false;  // swapchain recreated
        // NOTE: the overlay's Vulkan objects are built inside
        // detour_create_swapchain_game (the game is not presenting then).
        // Creating pipelines here would deadlock the NVIDIA driver.
        // Diagnostic opt-out: `touch /tmp/cs2_no_overlay` skips the ImGui
        // overlay (isolates whether present-hook rendering affects the game).
        static bool no_overlay = false;
        static auto last_chk = std::chrono::steady_clock::now();
        if (std::chrono::steady_clock::now() - last_chk > std::chrono::seconds(1)) {
            last_chk = std::chrono::steady_clock::now();
            no_overlay = static_cast<bool>(std::ifstream("/tmp/cs2_no_overlay"));
        }
        if (g_ready && !no_overlay) render_frame(queue, idx);
        else if (!g_format_warned_ && g_format == VK_FORMAT_UNDEFINED) {
            g_format_warned_ = true;
            log_("vk_hook: swapchain format unknown - the overlay was likely injected "
                 "after the game created its swapchain. Recreate the swapchain "
                 "(join a match / toggle fullscreen) or inject earlier.\n");
        }
    }
    const VkResult result = real_present(queue, pi);
    in_detour = false;
    return result;
}

namespace vk_hook {

void install() {
    if (g_hooked) return;
    void* vk = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!vk) {
        std::fprintf(stderr, "vk_hook: dlopen libvulkan failed\n");
        return;
    }
    real_igpa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(vk, "vkGetInstanceProcAddr"));
    real_gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(dlsym(vk, "vkGetDeviceProcAddr"));
    hook64::install(h_create_instance, dlsym(vk, "vkCreateInstance"),
                    reinterpret_cast<void*>(detour_create_instance));
    hook64::install(h_create_device, dlsym(vk, "vkCreateDevice"),
                    reinterpret_cast<void*>(detour_create_device));
    g_hooked = true;
    std::fprintf(stderr, "vk_hook: loader resolved, installing ICD hooks\n");
    install_icd_hook();
}

bool installed() { return g_hooked; }

void uninstall() {
    hook64::uninstall(h_icd_present);
    hook64::uninstall(h_icd_swapchain);
    hook64::uninstall(h_create_instance);
    hook64::uninstall(h_create_device);
    g_hooked = false;
    std::fprintf(stderr, "vk_hook: uninstalled\n");
}

}  // namespace vk_hook
