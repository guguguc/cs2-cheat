#include "input_x11.h"

#include "logger.h"
#include "overlay_ctx.h"

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/extensions/XInput2.h>

#include "imgui.h"

#include <unistd.h>

#include <cstdio>

namespace {
Display* dpy = nullptr;
Window root = 0;
Window game_win = 0;
KeyCode key_p = 0;
KeyCode key_f1 = 0;
KeyCode key_aim = 0;
int xi_opcode = 0;
bool xi_ok = false;
bool prev_buttons[3] = {false, false, false};
double raw_x = 0, raw_y = 0;
double last_qx = 0, last_qy = 0;
bool panel_was_open = false;

// Recursively searches the window tree for a window whose _NET_WM_PID
// matches our pid. GNOME/Mutter reparents X11 windows under a
// "mutter-x11-frames" decoration window, so the game window is NOT a direct
// child of the root - a single-level XQueryTree walk misses it (F1/input
// dead on GNOME, fine on Hyprland which does not reparent). DFS covers both.
Window find_game_window_recursive(Window w, pid_t self) {
    Window root_ret = 0, parent = 0;
    Window* children = nullptr;
    unsigned n = 0;
    // Check the window itself first.
    Atom pid_atom = XInternAtom(dpy, "_NET_WM_PID", True);
    if (pid_atom != None) {
        Atom type = None;
        int format = 0;
        unsigned long nitems = 0, after = 0;
        unsigned char* data = nullptr;
        if (XGetWindowProperty(dpy, w, pid_atom, 0, 1, False, XA_CARDINAL, &type,
                               &format, &nitems, &after, &data) == Success &&
            data && nitems == 1 && format == 32 &&
            *reinterpret_cast<unsigned long*>(data) == static_cast<unsigned long>(self)) {
            if (data) XFree(data);
            return w;
        }
        if (data) XFree(data);
    }
    if (!XQueryTree(dpy, w, &root_ret, &parent, &children, &n)) {
        if (children) XFree(children);
        return 0;
    }
    Window found = 0;
    for (unsigned i = 0; i < n && !found; ++i)
        found = find_game_window_recursive(children[i], self);
    if (children) XFree(children);
    return found;
}

Window find_game_window() {
    const pid_t self = getpid();
    return find_game_window_recursive(root, self);
}

}  // namespace

namespace input_x11 {

bool init() {
    if (dpy) return true;
    dpy = XOpenDisplay(nullptr);
    if (!dpy) return false;
    root = DefaultRootWindow(dpy);
    // XInput2 raw motion: reports mouse deltas even when the game uses
    // relative/raw input and leaves the OS cursor frozen.
    int evt = 0, err = 0;
    if (XQueryExtension(dpy, "XInputExtension", &xi_opcode, &evt, &err)) {
        XIEventMask mask;
        mask.deviceid = XIAllMasterDevices;
        mask.mask_len = XIMaskLen(XI_RawMotion);
        mask.mask = static_cast<unsigned char*>(calloc(mask.mask_len, 1));
        XISetMask(mask.mask, XI_RawMotion);
        if (XISelectEvents(dpy, root, &mask, 1) == Success) xi_ok = true;
        free(mask.mask);
    }
    game_win = find_game_window();
    key_p = XKeysymToKeycode(dpy, XK_p);
    key_f1 = XKeysymToKeycode(dpy, XK_F1);
    // 'x' is unbound by default in CS2 (avoid 'a' = strafe-left).
    key_aim = XKeysymToKeycode(dpy, XK_x);
    int grab_p = 0, grab_f1 = 0, grab_aim = 0;
    if (key_p && game_win)
        grab_p = XGrabKey(dpy, key_p, AnyModifier, root, True, GrabModeAsync, GrabModeAsync);
    if (key_f1 && game_win)
        grab_f1 = XGrabKey(dpy, key_f1, AnyModifier, root, True, GrabModeAsync, GrabModeAsync);
    if (key_aim && game_win)
        grab_aim = XGrabKey(dpy, key_aim, AnyModifier, root, True, GrabModeAsync, GrabModeAsync);
    XSync(dpy, False);
    Logger::instance().log("input_x11: init dpy=%p root=0x%lx game_win=0x%lx xi_ok=%d key_p=%d key_f1=%d key_aim=%d grab_p=%d grab_f1=%d grab_aim=%d\n",
                           static_cast<void*>(dpy), static_cast<unsigned long>(root),
                           static_cast<unsigned long>(game_win), xi_ok ? 1 : 0,
                           key_p, key_f1, key_aim, grab_p, grab_f1, grab_aim);
    return game_win != 0;
}

void poll(ImGuiIO& io) {
    if (!dpy) return;
    if (!game_win) game_win = find_game_window();

    // Keyboard: F1 (or 'p') toggles the panel (global grab). Also accumulate
    // XInput2 raw mouse deltas.
    double dx = 0, dy = 0;
    XEvent ev;
    while (XPending(dpy)) {
        XNextEvent(dpy, &ev);
        if (ev.type == KeyPress) {
            if (ev.xkey.keycode == key_f1 || ev.xkey.keycode == key_p)
                g_ctx.panel_open = !g_ctx.panel_open;
        } else if (xi_ok && ev.type == GenericEvent) {
            XGenericEventCookie* cookie = &ev.xcookie;
            if (XGetEventData(dpy, cookie) && cookie->extension == xi_opcode) {
                if (cookie->evtype == XI_RawMotion) {
                    XIRawEvent* raw = static_cast<XIRawEvent*>(cookie->data);
                    if (raw->valuators.mask_len > 0) {
                        double v[2] = {0, 0};
                        const double* vals = raw->raw_values;
                        int i = 0;
                        for (int a = 0; a < raw->valuators.mask_len * 8 && i < 2; ++a) {
                            if (XIMaskIsSet(raw->valuators.mask, a)) {
                                v[i++] = vals ? vals[a] : 0;
                            }
                        }
                        dx = v[0];
                        dy = v[1];
                    }
                }
                XFreeEventData(dpy, cookie);
            }
        }
    }


    // X key: edge-triggered toggle for the aimbot (independent of the menu
    // checkbox - either one activates aiming).
    char keys[32] = {0};
    if (key_aim)
        XQueryKeymap(dpy, keys);
    const bool key_down =
        key_aim != 0 && ((keys[key_aim >> 3] >> (key_aim & 7)) & 1) != 0;
    static bool prev_key_down = false;
    if (key_down && !prev_key_down) {
        // X toggles BOTH the menu checkbox and the hotkey state so they stay
        // in sync: on = on everywhere, off = off everywhere.
        g_ctx.aim_on = !g_ctx.aim_on;
        g_ctx.aim_toggle = g_ctx.aim_on;
    }
    prev_key_down = key_down;

    // Pointer position + buttons.
    Window r1 = 0, r2 = 0;
    int rx = 0, ry = 0, wx = 0, wy = 0;
    unsigned mask = 0;
    if (!game_win) return;
    XQueryPointer(dpy, root, &r1, &r2, &rx, &ry, &wx, &wy, &mask);
    int gx = 0, gy = 0;
    Window child = 0;
    XTranslateCoordinates(dpy, root, game_win, rx, ry, &gx, &gy, &child);
    last_qx = static_cast<double>(gx);
    last_qy = static_cast<double>(gy);

    // When the panel opens, seed the raw cursor at the absolute position;
    // while it is open, follow the raw deltas (the game may keep the OS cursor
    // frozen in raw-input mode).
    if (g_ctx.panel_open && !panel_was_open) {
        raw_x = last_qx;
        raw_y = last_qy;
    }
    if (g_ctx.panel_open) {
        raw_x += dx;
        raw_y += dy;
        io.AddMousePosEvent(static_cast<float>(raw_x), static_cast<float>(raw_y));
    } else {
        io.AddMousePosEvent(static_cast<float>(last_qx), static_cast<float>(last_qy));
    }
    panel_was_open = g_ctx.panel_open;

    const bool cur[3] = {static_cast<bool>(mask & Button1Mask),
                         static_cast<bool>(mask & Button2Mask),
                         static_cast<bool>(mask & Button3Mask)};
    for (int b = 0; b < 3; ++b) {
        if (cur[b] != prev_buttons[b]) io.AddMouseButtonEvent(b, cur[b]);
        prev_buttons[b] = cur[b];
    }
}

void shutdown() {
    if (!dpy) return;
    if (key_p && root)
        XUngrabKey(dpy, key_p, AnyModifier, root);
    if (key_f1 && root)
        XUngrabKey(dpy, key_f1, AnyModifier, root);
    if (key_aim && root)
        XUngrabKey(dpy, key_aim, AnyModifier, root);
    XSync(dpy, False);
    XCloseDisplay(dpy);
    dpy = nullptr;
    root = 0;
    game_win = 0;
    xi_ok = false;
    Logger::instance().log("input_x11: closed\n");
}

bool raw_tracking() { return xi_ok; }

}  // namespace input_x11
