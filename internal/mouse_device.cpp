#include "mouse_device.h"
#include "cfg.h"
#include "logger.h"
#include "overlay_ctx.h"

#include <fcntl.h>
#include <linux/uinput.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

// deadlocked: 1 / (sensitivity * 0.022) = counts per degree.
float counts_per_deg(float sens) { return 1.0f / (sens * 0.022f); }

}  // namespace

MouseDevice g_mouse;

// Live sensitivity: convar "sensitivity" (+0x58), defaulting to the config
// value; multiplied by the pawn's m_flFOVSensitivityAdjust (zoom scaling),
// exactly like deadlocked's get_sensitivity() * fov_multiplier().
float MouseDevice::live_sensitivity() const {
    float sens = Config::instance().offsets.sensitivity;
    if (sens_convar_) {
        const float v = *reinterpret_cast<const float*>(sens_convar_ + 0x58);
        if (v > 0.01f) sens = v;
    }
    if (local_pawn_) {
        const float fm = *reinterpret_cast<const float*>(
            local_pawn_ + Config::instance().offsets.m_flFOVSensitivityAdjust);
        if (fm > 0.01f) sens *= fm;
    }
    return sens;
}

void MouseDevice::move_counts(int dx, int dy) {
    if (fd_ < 0) return;
    input_event ev{};
    ev.type = EV_REL;
    ev.code = REL_X;
    ev.value = dx;
    ::write(fd_, &ev, sizeof ev);
    ev.type = EV_SYN;
    ev.code = SYN_REPORT;
    ev.value = 0;
    ::write(fd_, &ev, sizeof ev);
    ev.type = EV_REL;
    ev.code = REL_Y;
    ev.value = dy;
    ::write(fd_, &ev, sizeof ev);
    ev.type = EV_SYN;
    ev.code = SYN_REPORT;
    ev.value = 0;
    ::write(fd_, &ev, sizeof ev);
}

bool MouseDevice::init() {
    if (fd_ >= 0) return true;
    fd_ = ::open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd_ < 0) {
        Logger::instance().error("uinput: open /dev/uinput failed (%d); add user to the uinput "
                                 "group or run with sudo\n", errno);
        return false;
    }
    ioctl(fd_, UI_SET_EVBIT, EV_KEY);
    ioctl(fd_, UI_SET_EVBIT, EV_REL);
    ioctl(fd_, UI_SET_KEYBIT, BTN_LEFT);
    ioctl(fd_, UI_SET_KEYBIT, BTN_RIGHT);
    ioctl(fd_, UI_SET_KEYBIT, BTN_MIDDLE);
    ioctl(fd_, UI_SET_RELBIT, REL_X);
    ioctl(fd_, UI_SET_RELBIT, REL_Y);

    uinput_setup setup{};
    setup.id.bustype = BUS_USB;
    setup.id.vendor = 0x0451;   // Texas Instruments
    setup.id.product = 0xe008;  // TI-84 calculator (same disguise as deadlocked)
    setup.id.version = 1;
    std::strncpy(setup.name, "TI-84 Plus Silver Calculator",
                 UINPUT_MAX_NAME_SIZE - 1);
    if (ioctl(fd_, UI_DEV_SETUP, &setup) < 0 ||
        ioctl(fd_, UI_DEV_CREATE) < 0) {
        Logger::instance().error("uinput: UI_DEV_CREATE failed (%d)\n", errno);
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    Logger::instance().log("uinput: virtual mouse created (sens %.1f, %.3f counts/deg)\n",
                           Config::instance().offsets.sensitivity,
                           counts_per_deg(Config::instance().offsets.sensitivity));
    return true;
}

void MouseDevice::set_input_obj(std::uintptr_t input_obj) { input_obj_ = input_obj; }
void MouseDevice::set_sensitivity_convar(std::uintptr_t addr) { sens_convar_ = addr; }
void MouseDevice::set_local_pawn(std::uintptr_t pawn) { local_pawn_ = pawn; }

void MouseDevice::move_to(float target_pitch, float target_yaw) {
    // Lazily create the virtual mouse on first use (the user is aiming), so
    // no extra input device exists during normal play - a hotplugged pointer
    // device at inject time made the game's raw input lose the real mouse.
    if (fd_ < 0 && !init()) return;
    if (!input_obj_) return;

    // Live view angles live in the input object at +0x9C (QAngle pitch,yaw,roll).
    const auto* va = reinterpret_cast<const float*>(
        input_obj_ + Config::instance().offsets.viewAngleOffset);
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

    // deadlocked movement: angle error -> counts (45.45 = 1/0.022), divided by
    // (smooth+1), then an inertia low-pass filter so the crosshair glides
    // instead of jumping. Live sensitivity includes the zoom multiplier.
    const float cpd = counts_per_deg(live_sensitivity());
    const float smooth = std::clamp(g_ctx.aim_smooth, 1.f, 20.f);
    const float target_dx = -d_yaw * cpd / (smooth + 1.f);
    const float target_dy = d_pitch * cpd / (smooth + 1.f);
    inertia_dx_ += (target_dx - inertia_dx_) * 0.5f;
    inertia_dy_ += (target_dy - inertia_dy_) * 0.5f;
    int dx = static_cast<int>(inertia_dx_);
    int dy = static_cast<int>(inertia_dy_);
    if (dx == 0 && dy == 0) return;
    ++move_log_;
    if ((move_log_ & 0x3F) == 0)  // log every 64th steer to avoid spam
        Logger::instance().log("uinput: steer #%llu dx=%d dy=%d\n",
                               static_cast<unsigned long long>(move_log_), dx, dy);
    move_counts(dx, dy);
    // Aiming only: the triggerbot owns firing (deadlocked separates them).
}

void MouseDevice::move_counts_raw(int dx, int dy) {
    if (fd_ < 0) return;
    if (dx == 0 && dy == 0) return;
    move_counts(dx, dy);
}

void MouseDevice::set_fire(bool on) {
    if (fd_ < 0 || on == firing_) return;
    firing_ = on;
    input_event ev{};
    ev.type = EV_KEY;
    ev.code = BTN_LEFT;
    ev.value = on ? 1 : 0;
    ::write(fd_, &ev, sizeof ev);
    ev.type = EV_SYN;
    ev.code = SYN_REPORT;
    ev.value = 0;
    ::write(fd_, &ev, sizeof ev);
}

void MouseDevice::flush() {
    if (fd_ < 0) return;
    input_event ev{};
    ev.type = EV_REL;
    ev.code = REL_X;
    ev.value = 0;
    ::write(fd_, &ev, sizeof ev);
    ev.type = EV_SYN;
    ev.code = SYN_REPORT;
    ev.value = 0;
    ::write(fd_, &ev, sizeof ev);
}

bool MouseDevice::active() const { return fd_ >= 0; }

void MouseDevice::shutdown() {
    if (fd_ >= 0) {
        Logger::instance().log("uinput: device destroyed\n");
        ioctl(fd_, UI_DEV_DESTROY);
        ::close(fd_);
        fd_ = -1;
    }
}
