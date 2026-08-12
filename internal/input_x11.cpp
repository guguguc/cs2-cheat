#include "input_x11.h"

#include "overlay_ctx.h"

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/extensions/XInput2.h>

#include "imgui.h"

#include <unistd.h>

namespace {

Display* dpy = nullptr;
Window root = 0;
Window game_win = 0;
KeyCode key_p = 0;
KeyCode key_f1 = 0;
KeyCode key_aim = 0;
bool prev_buttons[3] = {false, false, false};
int xi_opcode = 0;
bool xi_ok = false;
double raw_x = 0, raw_y = 0;
double last_qx = 0, last_qy = 0;
bool panel_was_open = false;

Window find_game_window() {
    Window root_ret = 0, parent = 0;
    Window* children = nullptr;
    unsigned n = 0;
    if (!XQueryTree(dpy, root, &root_ret, &parent, &children, &n)) return 0;
    const pid_t self = getpid();
    Window found = 0;
    for (unsigned i = 0; i < n && !found; ++i) {
        Atom pid_atom = XInternAtom(dpy, "_NET_WM_PID", True);
        if (pid_atom == None) continue;
        Atom type = None;
        int format = 0;
        unsigned long nitems = 0, after = 0;
        unsigned char* data = nullptr;
        if (XGetWindowProperty(dpy, children[i], pid_atom, 0, 1, False, XA_CARDINAL, &type,
                               &format, &nitems, &after, &data) == Success &&
            data && nitems == 1 && format == 32 &&
            *reinterpret_cast<unsigned long*>(data) == static_cast<unsigned long>(self)) {
            found = children[i];
        }
        if (data) XFree(data);
    }
    if (children) XFree(children);
    return found;
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
    if (key_p && game_win)
        XGrabKey(dpy, key_p, AnyModifier, root, True, GrabModeAsync, GrabModeAsync);
    if (key_f1 && game_win)
        XGrabKey(dpy, key_f1, AnyModifier, root, True, GrabModeAsync, GrabModeAsync);
    if (key_aim && game_win)
        XGrabKey(dpy, key_aim, AnyModifier, root, True, GrabModeAsync, GrabModeAsync);
    XSync(dpy, False);
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

    // Authoritative aim-key state: XQueryKeymap reflects the physical key
    // every frame, so a lost KeyRelease can never leave the aimbot stuck on.
    char keys[32] = {0};
    if (key_aim)
        XQueryKeymap(dpy, keys);
    g_ctx.aim_hold =
        key_aim != 0 && ((keys[key_aim >> 3] >> (key_aim & 7)) & 1) != 0;

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

bool raw_tracking() { return xi_ok; }

}  // namespace input_x11
