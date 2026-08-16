#include "game_loop.h"
#include "input_x11.h"
#include "logger.h"
#include "mouse_device.h"
#include "overlay_ctx.h"
#include "vk_hook.h"

#include <X11/Xlib.h>

#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <thread>

SharedCtx g_ctx;

namespace {

std::atomic<bool> g_run{true};
// Set by the data thread right before it exits (all return paths), so
// unload_self can wait for it before the library is dlclosed. Without this
// the unloader used to race the still-running thread and dlclose could unmap
// the library under it.
std::atomic<bool> g_thread_done{false};

}  // namespace

__attribute__((constructor)) static void so_entry() {
    Logger::instance().error("cs2_internal: loaded into pid %d\n", getpid());
    Logger::instance().log("=== cs2_internal loaded pid=%d ===\n", getpid());
    // Multiple threads use Xlib connections (input_x11 + xtest_aim).
    XInitThreads();
    input_x11::init();
    std::thread([] {
        GameLoop loop(g_run);
        if (loop.init()) loop.run();
        g_thread_done.store(true);
    }).detach();
    g_vk_hook.install();
}

__attribute__((destructor)) static void so_exit() {
    g_run.store(false);
    Logger::instance().error("cs2_internal: unloaded\n");
}

// Called remotely by the unloader tool (injector_call). Stops the data
// thread, restores the Vulkan hooks, destroys the uinput device and releases
// the X11 connection so the library can be dlclosed safely.
extern "C" void unload_self() {
    g_run.store(false);
    // Wait for the data thread to actually exit (it polls g_run every ~16 ms).
    // Previous code only waited for the uinput device to go inactive, so
    // dlclose could unmap the library while the thread was still running.
    for (int i = 0; i < 400; ++i) {  // up to 2 s
        if (g_thread_done.load()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    // Stop rendering first so no other thread can touch the X11 connection
    // while we close it below.
    g_vk_hook.uninstall();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    g_mouse.shutdown();
    input_x11::shutdown();
    Logger::instance().error("cs2_internal: unload_self done (thread_done=%d)\n",
                             g_thread_done.load() ? 1 : 0);
}
