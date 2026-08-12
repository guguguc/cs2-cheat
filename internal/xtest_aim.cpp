#include "xtest_aim.h"

#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>

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

Display* g_dpy = nullptr;
std::uintptr_t g_input_obj = 0;
bool g_firing = false;
bool g_ready = false;

// Game sensitivity: counts per degree = 1 / (sensitivity * 0.022).
constexpr float kSens = 2.5f;
constexpr float kCountsPerDeg = 1.0f / (kSens * 0.022f);
// Move this fraction of the remaining angle error per frame, capped.
constexpr float kFracPerFrame = 0.5f;
constexpr int kMaxPerFrame = 180;
constexpr float kLockDeg = 1.2f;  // fire when residual error is below this

}  // namespace

namespace xtest_aim {

bool init() {
    if (g_dpy) return true;
    g_dpy = XOpenDisplay(nullptr);
    if (!g_dpy) {
        log_("xtest: cannot open display\n");
        return false;
    }
    int evt = 0, err = 0, major = 0, minor = 0;
    if (!XTestQueryExtension(g_dpy, &evt, &err, &major, &minor)) {
        log_("xtest: XTest extension unavailable\n");
        XCloseDisplay(g_dpy);
        g_dpy = nullptr;
        return false;
    }
    XTestGrabControl(g_dpy, True);  // route fake input through the server
    g_ready = true;
    log_("xtest: ready (sens %.1f)\n", kSens);
    return true;
}

void set_input_obj(std::uintptr_t input_obj) { g_input_obj = input_obj; }

void move_to(float target_pitch, float target_yaw) {
    if (!g_dpy && !init()) return;
    if (!g_ready || !g_input_obj) return;

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
    if (dx != 0 || dy != 0) {
        XTestFakeRelativeMotionEvent(g_dpy, dx, dy, 0);
        XFlush(g_dpy);
    }

    // Auto-fire when locked onto the target.
    set_fire(err < kLockDeg);
}

void set_fire(bool on) {
    if (!g_dpy || !g_ready || on == g_firing) return;
    g_firing = on;
    XTestFakeButtonEvent(g_dpy, 1, on ? True : False, 0);
    XFlush(g_dpy);
}

void shutdown() {
    if (g_dpy) {
        if (g_firing) {
            XTestFakeButtonEvent(g_dpy, 1, False, 0);
            XFlush(g_dpy);
            g_firing = false;
        }
        XCloseDisplay(g_dpy);
        g_dpy = nullptr;
    }
    g_ready = false;
}

}  // namespace xtest_aim
