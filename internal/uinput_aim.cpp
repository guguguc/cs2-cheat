#include "uinput_aim.h"

#include <fcntl.h>
#include <linux/uinput.h>
#include <unistd.h>

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
bool g_firing = false;
std::uint64_t g_move_log = 0;

// Game sensitivity: counts per degree = 1 / (sensitivity * 0.022). The 0.022
// is m_yaw (default). Adjust kSens to match the player's in-game sensitivity.
constexpr float kSens = 2.5f;
constexpr float kCountsPerDeg = 1.0f / (kSens * 0.022f);
// Movement: move this fraction of the remaining angle error per frame, capped
// at kMaxPerFrame counts so far targets snap fast but not instantly.
constexpr float kFracPerFrame = 0.5f;
constexpr int kMaxPerFrame = 180;
constexpr float kLockDeg = 1.2f;  // fire when residual error is below this

void move_counts(int dx, int dy) {
    if (g_fd < 0) return;
    input_event ev{};
    ev.type = EV_REL;
    ev.code = REL_X;
    ev.value = dx;
    ::write(g_fd, &ev, sizeof ev);
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
         kSens, kCountsPerDeg);
    return true;
}

void set_input_obj(std::uintptr_t input_obj) { g_input_obj = input_obj; }

void move_to(float target_pitch, float target_yaw) {
    if (g_fd < 0 || !g_input_obj) return;

    // Live view angles live in the input object at +0x9C (QAngle pitch,yaw,roll).
    const auto* va = reinterpret_cast<const float*>(g_input_obj + 0x9C);
    const float cur_pitch = va[0];
    const float cur_yaw = va[1];

    float d_yaw = target_yaw - cur_yaw;
    while (d_yaw > 180.f) d_yaw -= 360.f;
    while (d_yaw < -180.f) d_yaw += 360.f;
    const float d_pitch = target_pitch - cur_pitch;
    const float err = std::sqrt(d_pitch * d_pitch + d_yaw * d_yaw);

    // Source convention: mouse right (-x) turns right = yaw decreases; mouse
    // down (+y) looks down = pitch increases. Flip signs if inverted.
    int dx = static_cast<int>(-d_yaw * kFracPerFrame * kCountsPerDeg);
    int dy = static_cast<int>(d_pitch * kFracPerFrame * kCountsPerDeg);
    if (dx > kMaxPerFrame) dx = kMaxPerFrame;
    if (dx < -kMaxPerFrame) dx = -kMaxPerFrame;
    if (dy > kMaxPerFrame) dy = kMaxPerFrame;
    if (dy < -kMaxPerFrame) dy = -kMaxPerFrame;
    if (dx == 0 && dy == 0) return;
    ++g_move_log;
    if (g_move_log <= 6 || (g_move_log % 1000) == 0)
        log_("uinput: steer dx=%d dy=%d (cur %.1f %.1f -> tgt %.1f %.1f)\n",
             dx, dy, static_cast<double>(cur_pitch), static_cast<double>(cur_yaw),
             static_cast<double>(target_pitch), static_cast<double>(target_yaw));
    move_counts(dx, dy);

    // Auto-fire when locked onto the target.
    set_fire(err < kLockDeg);
}

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

void shutdown() {
    if (g_fd >= 0) {
        ioctl(g_fd, UI_DEV_DESTROY);
        ::close(g_fd);
        g_fd = -1;
    }
}

}  // namespace uinput_aim
