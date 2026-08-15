// Standalone verification of the inline-hook + trampoline machinery against
// the REAL Vulkan loader. No game needed: it patches libvulkan inside this
// test process, then creates an instance + device through the loader stubs
// (exactly the path the game uses) and checks that the detours fire and the
// trampolines return control correctly.
#include "hook_x64.h"

#include <vulkan/vulkan.h>

#include <dlfcn.h>

#include <cstdio>

namespace {

PFN_vkGetInstanceProcAddr real_igpa = nullptr;
hook64::Hook h_inst;
hook64::Hook h_dev;
int inst_calls = 0;
int dev_calls = 0;

using CreateInstanceFn = VkResult (*)(const VkInstanceCreateInfo*,
                                      const VkAllocationCallbacks*, VkInstance*);
using CreateDeviceFn = VkResult (*)(VkPhysicalDevice, const VkDeviceCreateInfo*,
                                    const VkAllocationCallbacks*, VkDevice*);

extern "C" VkResult det_inst(const VkInstanceCreateInfo* ci, const VkAllocationCallbacks* ac,
                             VkInstance* out) {
    ++inst_calls;
    const VkResult r = reinterpret_cast<CreateInstanceFn>(h_inst.trampoline())(ci, ac, out);
    std::printf("  [detour] vkCreateInstance -> %d (trampoline ok)\n", r);
    return r;
}

extern "C" VkResult det_dev(VkPhysicalDevice pd, const VkDeviceCreateInfo* ci,
                            const VkAllocationCallbacks* ac, VkDevice* out) {
    ++dev_calls;
    const VkResult r = reinterpret_cast<CreateDeviceFn>(h_dev.trampoline())(pd, ci, ac, out);
    std::printf("  [detour] vkCreateDevice -> %d device=%p\n", r, static_cast<void*>(*out));
    return r;
}

}  // namespace

// --- RIP-relative relocation check ------------------------------------------
static volatile int g_rip_marker = 42;

__attribute__((noinline)) static int rip_fn(int x) {
    return g_rip_marker + x;  // compiles to `mov eax, [rip+disp]` in the prologue
}

static int test_rip_relocation() {
    hook64::Hook h;
    if (!h.install(reinterpret_cast<void*>(&rip_fn),
                   reinterpret_cast<void*>(&rip_fn))) {
        std::printf("  [rip] hook install failed\n");
        return 1;
    }
    // Call the ORIGINAL through the trampoline (as a detour would).
    using Fn = int (*)(int);
    const int r = reinterpret_cast<Fn>(h.trampoline())(5);
    std::printf("  [rip] trampoline(5) = %d (expect 47)\n", r);
    return r == 47 ? 0 : 2;
}

int main() {
    const int rip_ok = test_rip_relocation();
    void* vk = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!vk) {
        std::printf("FAIL: dlopen libvulkan: %s\n", dlerror());
        return 1;
    }
    real_igpa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(vk, "vkGetInstanceProcAddr"));

    std::printf("installing hooks...\n");
    const bool ok_inst =
        h_inst.install(dlsym(vk, "vkCreateInstance"),
                        reinterpret_cast<void*>(det_inst));
    const bool ok_dev =
        h_dev.install(dlsym(vk, "vkCreateDevice"),
                        reinterpret_cast<void*>(det_dev));
    std::printf("hooks: vkCreateInstance=%d vkCreateDevice=%d\n", ok_inst, ok_dev);
    if (!ok_inst || !ok_dev) return 2;

    // ---- exercise the patched path exactly like the game does ----
    const auto create_instance =
        reinterpret_cast<PFN_vkCreateInstance>(dlsym(vk, "vkCreateInstance"));
    VkApplicationInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &ai;
    VkInstance inst = VK_NULL_HANDLE;
    VkResult r = create_instance(&ici, nullptr, &inst);
    std::printf("create_instance -> %d (detour fired: %d)\n", r, inst_calls);
    if (r != VK_SUCCESS || inst_calls != 1) return 3;

    const auto enum_phys =
        reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(real_igpa(inst, "vkEnumeratePhysicalDevices"));
    std::uint32_t count = 0;
    enum_phys(inst, &count, nullptr);
    if (count == 0) {
        std::printf("no physical devices (headless?) - device detour not exercisable\n");
        return 0;
    }
    VkPhysicalDevice phys[4] = {};
    if (count > 4) count = 4;
    enum_phys(inst, &count, phys);

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
    const auto create_device =
        reinterpret_cast<PFN_vkCreateDevice>(real_igpa(inst, "vkCreateDevice"));
    VkDevice dev = VK_NULL_HANDLE;
    r = create_device(phys[0], &dci, nullptr, &dev);
    std::printf("create_device -> %d (detour fired: %d)\n", r, dev_calls);
    if (dev_calls != 1) return 4;

    std::printf("SELFTEST %s (rip-reloc=%s)\n",
                rip_ok == 0 ? "PASS" : "FAIL", rip_ok == 0 ? "ok" : "bad");
    return rip_ok;
}
