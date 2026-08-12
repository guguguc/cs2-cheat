#include "aimbot.h"
#include "game.h"
#include "input_x11.h"
#include "memory.h"
#include "offsets.h"
#include "overlay_ctx.h"
#include "patterns.h"
#include "process.h"
#include "trigger.h"
#include "uinput_aim.h"
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

void data_thread() {
    Memory mem;
    mem.attach(getpid());
    patterns::Resolved off;
    if (!patterns::resolve(getpid(), off)) {
        log_("entry: pattern resolve failed; retrying in background\n");
        for (int i = 0; i < 20 && !off.ok; ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            patterns::resolve(getpid(), off);
        }
    }
    if (!off.ok) return;

    Game game;
    if (!game.attach(mem, off)) {
        log_("entry: game attach failed\n");
        return;
    }
    log_("entry: attached, offsets ok, client base 0x%llx\n",
         static_cast<unsigned long long>(game.client_base()));

    // Input object: the live view angles are mirrored at +0x9C.
    const std::uintptr_t input_obj =
        mem.read<std::uintptr_t>(game.client_base() + offsets::dwCSGOInput)
            .value_or(0);
    if (input_obj) {
        game.set_view_angle_source(input_obj + 0x9C);  // real view angles
        uinput_aim::set_input_obj(input_obj);
        // Virtual uinput mouse, created lazily on first aim, destroyed on
        // aim-key release (the "hotplug" configuration).
        game.set_angles_override([](const Vector3& a, bool) {
            uinput_aim::move_to(a.x, a.y);
        });
        log_("aim: uinput ready (input_obj=0x%llx)\n",
             static_cast<unsigned long long>(input_obj));
    } else {
        log_("input: no input object (dwCSGOInput null)\n");
        game.set_angles_override([](const Vector3&, bool) {});
    }

    Triggerbot trigger;
    while (g_run.load()) {
        game.update(mem);
        {
            std::lock_guard<std::mutex> lk(g_ctx.mtx);
            g_ctx.snap = game.snapshot();
            g_ctx.valid = game.local_pawn() != 0;
        }

        bool aim = false, trig = false;
        float fov = 12.f, sm = 6.f;
        {
            std::lock_guard<std::mutex> lk(g_ctx.mtx);
            aim = g_ctx.aim_on && g_ctx.aim_hold;
            trig = g_ctx.trigger_on;
            fov = g_ctx.aim_fov;
            sm = g_ctx.aim_smooth;
        }
        const bool steered = run_aimbot(game, mem, aim, fov, sm);

        if (steered)
            log_("aimbot: steered (aim=%d)\n", aim ? 1 : 0);
        static bool last_aim = false;
        if (aim != last_aim) {
            last_aim = aim;
            log_("aim: %s (aim_on=%d aim_hold=%d)\n", aim ? "ACTIVE" : "off",
                 g_ctx.aim_on ? 1 : 0, g_ctx.aim_hold ? 1 : 0);
        }
        if (!steered || g_ctx.panel_open) uinput_aim::set_fire(false);
        // Destroy the virtual device when the aim key is released.
        if (!aim) uinput_aim::shutdown();
        trigger.run(game, mem, trig);

        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
}

}  // namespace

__attribute__((constructor)) static void so_entry() {
    std::fprintf(stderr, "cs2_internal: loaded into pid %d\n", getpid());
    log_("=== cs2_internal loaded pid=%d ===\n", getpid());
    // Multiple threads use Xlib connections (input_x11 + xtest_aim).
    XInitThreads();
    input_x11::init();
    std::thread(data_thread).detach();
    vk_hook::install();
}

__attribute__((destructor)) static void so_exit() {
    g_run.store(false);
    std::fprintf(stderr, "cs2_internal: unloaded\n");
}
