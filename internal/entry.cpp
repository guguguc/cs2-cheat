#include "aimbot.h"
#include "cfg.h"
#include "game.h"
#include "input_x11.h"
#include "memory.h"
#include "mouse_device.h"
#include "offsets.h"
#include "overlay_ctx.h"
#include "rcs.h"
#include "patterns.h"
#include "process.h"
#include "process.h"
#include "trigger.h"
#include "vk_hook.h"

#include <X11/Xlib.h>

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdarg>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

SharedCtx g_ctx;

namespace {

std::atomic<bool> g_run{true};
// Set by the data thread right before it exits (all return paths), so
// unload_self can wait for it before the library is dlclosed. Without this
// the unloader used to race the still-running thread and dlclose could unmap
// the library under it.
std::atomic<bool> g_thread_done{false};

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

// Scans the ICvar convar list for "sensitivity" (deadlocked-style).
// libtier0.so exports VEngineCvar007 (interface table at base + kCvarIfaceOff);
// each convar object has a name pointer at +0x0 and its float value at +0x58.
std::uintptr_t find_sensitivity_convar(const Memory& mem) {
    const auto tier0 = module_base(mem.pid(), "libtier0.so");
    if (!tier0) { log_("convar: no libtier0\n"); return 0; }
    constexpr std::uintptr_t kCvarIfaceOff = 1423200;  // VEngineCvar007 (dumper 2026-08-13)
    const auto iface = mem.read<std::uintptr_t>(*tier0 + kCvarIfaceOff).value_or(0);
    if (!iface) { log_("convar: no cvar interface\n"); return 0; }
    const auto objects = mem.read<std::uintptr_t>(iface + 0x50).value_or(0);
    const std::uint32_t count = mem.read<std::uint32_t>(iface + 160).value_or(0);
    if (!objects || !count) { log_("convar: empty list (count=%u)\n", count); return 0; }
    for (std::uint32_t i = 0; i < count && i < 4096; ++i) {
        const auto obj = mem.read<std::uintptr_t>(objects + i * 16).value_or(0);
        if (!obj) break;
        const auto name_addr = mem.read<std::uintptr_t>(obj).value_or(0);
        if (!name_addr) continue;
        char buf[32] = {0};
        mem.read(name_addr, buf, sizeof(buf) - 1);
        if (std::string(buf) == "sensitivity") {
            log_("convar: sensitivity at 0x%llx\n", static_cast<unsigned long long>(obj));
            return obj;
        }
    }
    log_("convar: sensitivity not found (count=%u)\n", count);
    return 0;
}

// deadlocked check_bvh equivalent: every 200 ms, if the map name (read from
// global_vars+0x198, same as deadlocked current_map()) changed, reload the
// BVH. Reload only happens after entering a match; retries automatically
// until read_map succeeds (failed loads do not remember the map name).
std::string read_cstr(const Memory& mem, std::uintptr_t addr) {
    if (!addr) return {};
    char buf[128] = {0};
    mem.read(addr, buf, sizeof(buf) - 1);
    return std::string(buf);
}

void check_bvh(Aimbot& aimbot, const Memory& mem, const patterns::Resolved& off) {
    static std::string last_map;
    static auto last_check = std::chrono::steady_clock::now();
    if (std::chrono::steady_clock::now() - last_check < std::chrono::milliseconds(200)) return;
    last_check = std::chrono::steady_clock::now();
    if (!off.globalVars || !off.vphysWorld) return;
    const auto gv = mem.read<std::uintptr_t>(off.globalVars).value_or(0);
    if (!gv) return;
    const auto name_ptr = mem.read<std::uintptr_t>(gv + 0x198).value_or(0);
    const std::string map = read_cstr(mem, name_ptr);
    if (map == last_map) return;
    last_map = map;
    if (aimbot.init_bvh(off.vphysWorld, /*force=*/true)) {
        // loaded ok (fresh geometry for the new map)
    } else {
        last_map.clear();  // failed: retry next 200 ms cycle
    }
}

void data_thread() {
    log_("data_thread: start\n");
    Memory mem;
    mem.attach(getpid());
    patterns::OffsetResolver offsets;
    if (!offsets.attach(getpid())) {
        log_("entry: pattern resolve failed; retrying in background\n");
        for (int i = 0; i < 20 && !offsets.ok(); ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            offsets.attach(getpid());
        }
    }
    if (!offsets.ok()) {
        g_thread_done.store(true);
        return;
    }
    const patterns::Resolved& off = offsets.resolved();

    Game game(mem);
    if (!game.attach(off)) {
        log_("entry: game attach failed\n");
        g_thread_done.store(true);
        return;
    }
    log_("entry: attached, offsets ok, client base 0x%llx\n",
         static_cast<unsigned long long>(game.client_base()));

    // Input object: the live view angles are mirrored at +0x9C.
    const std::uintptr_t input_obj =
        mem.read<std::uintptr_t>(game.client_base() + Config::instance().offsets.dwCSGOInput)
            .value_or(0);
    if (input_obj) {
        game.set_view_angle_source(input_obj + Config::instance().offsets.viewAngleOffset);  // real view angles
        g_mouse.set_input_obj(input_obj);
        g_mouse.set_sensitivity_convar(find_sensitivity_convar(mem));
        // Create the virtual device lazily on first aim (in-match, so it never
        // collides with the game's raw-input init) and keep it alive (no
        // destroy -> no pointer-warp self-move on release).
        game.set_angles_override([](const Vector3& a, bool) {
            g_mouse.move_to(a.x, a.y);
        });
        log_("aim: uinput ready (input_obj=0x%llx)\n",
             static_cast<unsigned long long>(input_obj));
    } else {
        log_("input: no input object (dwCSGOInput null)\n");
        game.set_angles_override([](const Vector3&, bool) {});
    }

    Triggerbot trigger(game);
    Rcs rcs(game);
    Aimbot aimbot(game);
    while (g_run.load()) {
        game.update();
        check_bvh(aimbot, mem, off);  // deadlocked-style: 200 ms, map-change triggered
        {
            std::lock_guard<std::mutex> lk(g_ctx.mtx);
            g_ctx.snap = game.snapshot();
            g_ctx.valid = game.local_pawn() != 0;
            for (Player& p : g_ctx.snap.players) p.visible = aimbot.visible(p);
        }

        g_mouse.set_local_pawn(game.local_pawn());

        bool aim = false, trig = false, rcs_on = false;
        float fov = 12.f, sm = 6.f;
        float rcs_strength = 0.5f;
        {
            std::lock_guard<std::mutex> lk(g_ctx.mtx);
            aim = g_ctx.aim_on;  // menu checkbox and X key are kept in sync
            trig = g_ctx.trigger_on;
            rcs_on = g_ctx.rcs_on;
            rcs_strength = g_ctx.rcs_strength;
            fov = g_ctx.aim_fov;
            sm = g_ctx.aim_smooth;
        }
        const bool steered = aimbot.run(aim, fov, sm);
        {
            std::lock_guard<std::mutex> lk(g_ctx.mtx);
            g_ctx.aim_active = steered;  // screen hint: aiming right now
        }
        bool fire_now = false;

        if (steered)
            log_("aimbot: steered (aim=%d)\n", aim ? 1 : 0);
        static bool last_aim = false;
        if (aim != last_aim) {
            last_aim = aim;
            log_("aim: %s (aim_on=%d aim_toggle=%d)\n", aim ? "ACTIVE" : "off",
                 g_ctx.aim_on ? 1 : 0, g_ctx.aim_toggle ? 1 : 0);
        }
        rcs.run(rcs_on, rcs_strength);             // recoil control (independent)
        trigger.run(trig, fire_now);               // schedule (delayed shot)
        trigger.run_shoot();                       // drive the button

        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    g_thread_done.store(true);
    log_("data_thread: exited\n");
}

}  // namespace

__attribute__((constructor)) static void so_entry() {
    std::fprintf(stderr, "cs2_internal: loaded into pid %d\n", getpid());
    log_("=== cs2_internal loaded pid=%d ===\n", getpid());
    // Multiple threads use Xlib connections (input_x11 + xtest_aim).
    XInitThreads();
    input_x11::init();
    std::thread(data_thread).detach();
    g_vk_hook.install();
}

__attribute__((destructor)) static void so_exit() {
    g_run.store(false);
    std::fprintf(stderr, "cs2_internal: unloaded\n");
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
    std::fprintf(stderr, "cs2_internal: unload_self done (thread_done=%d)\n",
                 g_thread_done.load() ? 1 : 0);
}
