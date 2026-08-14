#include "uinput_aim.h"
#include "cfg.h"
#include "overlay_ctx.h"

#include <fcntl.h>
#include <linux/uinput.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

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

int g_fd = -1;
std::uintptr_t g_input_obj = 0;
std::uintptr_t g_sens_convar = 0;  // live "sensitivity" convar (+0x58 = float value)
std::uintptr_t g_local_pawn = 0;   // for m_flFOVSensitivityAdjust (zoom sens scale)
bool g_firing = false;
std::uint64_t g_move_log = 0;
// deadlocked-style inertia low-pass filter for the mouse movement.
float g_inertia_dx = 0.f;
float g_inertia_dy = 0.f;

constexpr float kLockDeg = 1.2f;  // fire when residual error is below this

// Live sensitivity: convar "sensitivity" (+0x58), defaulting to the config
// value; multiplied by the pawn's m_flFOVSensitivityAdjust (zoom scaling),
// exactly like deadlocked's get_sensitivity() * fov_multiplier().
float live_sensitivity() {
    float sens = cs2cfg().offsets.sensitivity;
    if (g_sens_convar) {
        const float v = *reinterpret_cast<const float*>(g_sens_convar + 0x58);
        if (v > 0.01f) sens = v;
    }
    if (g_local_pawn) {
        const float fm = *reinterpret_cast<const float*>(
            g_local_pawn + cs2cfg().offsets.m_flFOVSensitivityAdjust);
        if (fm > 0.01f) sens *= fm;
    }
    return sens;
}
// deadlocked: 1 / (sensitivity * 0.022) = counts per degree.
float counts_per_deg(float sens) { return 1.0f / (sens * 0.022f); }

void move_counts(int dx, int dy) {
    if (g_fd < 0) return;
    input_event ev{};
    ev.type = EV_REL;
    ev.code = REL_X;
    ev.value = dx;
    ::write(g_fd, &ev, sizeof ev);
    ev.type = EV_SYN;
    ev.code = SYN_REPORT;
    ev.value = 0;
    ::write(g_fd, &ev, sizeof ev);
    ev.type = EV_REL;
    ev.code = REL_Y;
    ev.value = dy;
    ::write(g_fd, &ev, sizeof ev);
    ev.type = EV_SYN;
    ev.code = SYN_REPORT;
    ev.value = 0;
    ::write(g_fd, &ev, sizeof ev);
}

}  // namespace

namespace uinput_aim {

bool init() {
    if (g_fd >= 0) return true;
    g_fd = ::open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (g_fd < 0) {
        log_("uinput: open /dev/uinput failed (%d); add user to the uinput "
             "group or run with sudo\n", errno);
        return false;
    }
    ioctl(g_fd, UI_SET_EVBIT, EV_KEY);
    ioctl(g_fd, UI_SET_EVBIT, EV_REL);
    ioctl(g_fd, UI_SET_KEYBIT, BTN_LEFT);
    ioctl(g_fd, UI_SET_KEYBIT, BTN_RIGHT);
    ioctl(g_fd, UI_SET_KEYBIT, BTN_MIDDLE);
    ioctl(g_fd, UI_SET_RELBIT, REL_X);
    ioctl(g_fd, UI_SET_RELBIT, REL_Y);

    uinput_setup setup{};
    setup.id.bustype = BUS_USB;
    setup.id.vendor = 0x0451;   // Texas Instruments
    setup.id.product = 0xe008;  // TI-84 calculator (same disguise as deadlocked)
    setup.id.version = 1;
    std::strncpy(setup.name, "TI-84 Plus Silver Calculator",
                 UINPUT_MAX_NAME_SIZE - 1);
    if (ioctl(g_fd, UI_DEV_SETUP, &setup) < 0 ||
        ioctl(g_fd, UI_DEV_CREATE) < 0) {
        log_("uinput: UI_DEV_CREATE failed (%d)\n", errno);
        ::close(g_fd);
        g_fd = -1;
        return false;
    }
    log_("uinput: virtual mouse created (sens %.1f, %.3f counts/deg)\n",
         cs2cfg().offsets.sensitivity, counts_per_deg(cs2cfg().offsets.sensitivity));
    return true;
}

void set_input_obj(std::uintptr_t input_obj) { g_input_obj = input_obj; }
void set_sensitivity_convar(std::uintptr_t addr) { g_sens_convar = addr; }
void set_local_pawn(std::uintptr_t pawn) { g_local_pawn = pawn; }

void move_to(float target_pitch, float target_yaw) {
    // Lazily create the virtual mouse on first use (the user is aiming), so
    // no extra input device exists during normal play - a hotplugged pointer
    // device at inject time made the game's raw input lose the real mouse.
    if (g_fd < 0 && !init()) return;
    if (!g_input_obj) return;

    // Live view angles live in the input object at +0x9C (QAngle pitch,yaw,roll).
    const auto* va = reinterpret_cast<const float*>(g_input_obj + cs2cfg().offsets.viewAngleOffset);
    const float cur_pitch = va[0];
    const float cur_yaw = va[1];

    float d_yaw = target_yaw - cur_yaw;
    while (d_yaw > 180.f) d_yaw -= 360.f;
    while (d_yaw < -180.f) d_yaw += 360.f;
    float d_pitch = target_pitch - cur_pitch;
    // deadlocked vec2_clamp: keep pitch within +-89 so we never flip past
    // straight-up/down.
    if (d_pitch > 89.f) d_pitch = 89.f;
    if (d_pitch < -89.f) d_pitch = -89.f;
    const float err = std::sqrt(d_pitch * d_pitch + d_yaw * d_yaw);

    // deadlocked movement: angle error -> counts (45.45 = 1/0.022), divided by
    // (smooth+1), then an inertia low-pass filter so the crosshair glides
    // instead of jumping. Live sensitivity includes the zoom multiplier.
    const float cpd = counts_per_deg(live_sensitivity());
    const float smooth = std::clamp(g_ctx.aim_smooth, 1.f, 20.f);
    const float target_dx = -d_yaw * cpd / (smooth + 1.f);
    const float target_dy = d_pitch * cpd / (smooth + 1.f);
    g_inertia_dx += (target_dx - g_inertia_dx) * 0.5f;
    g_inertia_dy += (target_dy - g_inertia_dy) * 0.5f;
    int dx = static_cast<int>(g_inertia_dx);
    int dy = static_cast<int>(g_inertia_dy);
    if (dx == 0 && dy == 0) return;
    ++g_move_log;
    if ((g_move_log & 0x3F) == 0)  // log every 64th steer to avoid spam
        log_("uinput: steer #%llu dx=%d dy=%d\n",
             static_cast<unsigned long long>(g_move_log), dx, dy);
    move_counts(dx, dy);
    // Aiming only: the triggerbot owns firing (deadlocked separates them).
}

void move_counts_raw(int dx, int dy) {
    if (g_fd < 0) return;
    if (dx == 0 && dy == 0) return;
    move_counts(dx, dy);
}

float live_sensitivity() { return ::live_sensitivity(); }

void set_fire(bool on) {
    if (g_fd < 0 || on == g_firing) return;
    g_firing = on;
    input_event ev{};
    ev.type = EV_KEY;
    ev.code = BTN_LEFT;
    ev.value = on ? 1 : 0;
    ::write(g_fd, &ev, sizeof ev);
    ev.type = EV_SYN;
    ev.code = SYN_REPORT;
    ev.value = 0;
    ::write(g_fd, &ev, sizeof ev);
}

void flush() {
    if (g_fd < 0) return;
    input_event ev{};
    ev.type = EV_REL;
    ev.code = REL_X;
    ev.value = 0;
    ::write(g_fd, &ev, sizeof ev);
    ev.type = EV_SYN;
    ev.code = SYN_REPORT;
    ev.value = 0;
    ::write(g_fd, &ev, sizeof ev);
}

bool active() { return g_fd >= 0; }

void shutdown() {
    if (g_fd >= 0) {
        log_("uinput: device destroyed\n");
        ioctl(g_fd, UI_DEV_DESTROY);
        ::close(g_fd);
        g_fd = -1;
    }
}

}  // namespace uinput_aim
