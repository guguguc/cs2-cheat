#include "overlay_imgui.h"

#include "config.h"
#include "hyprland.h"
#include "math.h"

// Must precede <GLFW/glfw3.h> (which pulls in the system <GL/gl.h>): the
// imgl3w loader header reuses glcorearb.h's PFNGL* typedefs.
#include <GL/glcorearb.h>
#include <GLFW/glfw3.h>
#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_opengl3_loader.h"  // imgl3wInit2 / GL3WGetProcAddressProc

#include <algorithm>
#include <cstdio>
#include <cstring>

#ifdef GLFW_EXPOSE_NATIVE_X11
#include <X11/Xlib.h>
#include <X11/extensions/shape.h>
#include <X11/keysym.h>
#include <GLFW/glfw3native.h>
#endif

namespace {

ImU32 team_color(int team, bool teammate) {
    if (teammate) return IM_COL32(170, 170, 170, 255);
    return team == 2 ? IM_COL32(255, 90, 90, 255) : IM_COL32(90, 190, 255, 255);
}

ImU32 health_color(int hp) {
    if (hp > 60) return IM_COL32(0, 255, 110, 255);
    if (hp > 30) return IM_COL32(255, 235, 60, 255);
    return IM_COL32(255, 60, 60, 255);
}

}  // namespace

bool ImGuiOverlay::init(const char* platform) {
    // Platform selection MUST happen before glfwInit(). On Hyprland we prefer
    // the X11 backend: the game itself is an XWayland window, and an X11
    // overlay gives us click-through (XShape) while Hyprland still renders
    // it above the fullscreen game.
    bool force_x11 = false;
    if (platform && *platform) {
        if (std::strcmp(platform, "x11") == 0) {
            glfwWindowHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
            force_x11 = true;
        } else if (std::strcmp(platform, "wayland") == 0) {
            glfwWindowHint(GLFW_PLATFORM, GLFW_PLATFORM_WAYLAND);
        }
    } else if (hypr::available()) {
        glfwWindowHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
        force_x11 = true;
    }

    if (!glfwInit()) {
        std::fprintf(stderr, "overlay: glfwInit() failed\n");
        return false;
    }
    hypr_ = hypr::available();
    const char* platform_name = "other";
    if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND) platform_name = "wayland";
    else if (glfwGetPlatform() == GLFW_PLATFORM_X11) platform_name = "x11";
    std::fprintf(stderr, "overlay: GLFW %s, platform=%s%s\n",
                 glfwGetVersionString(), platform_name,
                 force_x11 ? " (forced x11)" : "");

    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_TRUE);
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
    glfwWindowHint(GLFW_FLOATING, GLFW_TRUE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    glfwWindowHint(GLFW_FOCUSED, GLFW_FALSE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

    // Prefer the game window's exact geometry so world-to-screen matches the
    // overlay pixel-for-pixel; fall back to the monitor size.
    if (hypr_ && hypr::find_game_window(game_x_, game_y_, game_w_, game_h_) &&
        game_w_ > 0 && game_h_ > 0) {
        fb_w_ = game_w_;
        fb_h_ = game_h_;
        std::fprintf(stderr, "overlay: matching game window %dx%d @ %d,%d\n",
                     game_w_, game_h_, game_x_, game_y_);
    } else {
        const GLFWvidmode* vm = glfwGetVideoMode(glfwGetPrimaryMonitor());
        if (!vm) {
            std::fprintf(stderr, "overlay: no video mode (no display?)\n");
            glfwTerminate();
            return false;
        }
        fb_w_ = vm->width;
        fb_h_ = vm->height;
    }

    win_ = glfwCreateWindow(fb_w_, fb_h_, "cs2-cheat overlay", nullptr, nullptr);
    if (!win_) {
        std::fprintf(stderr, "overlay: glfwCreateWindow() failed (no GLX/EGL context?)\n");
        glfwTerminate();
        return false;
    }
    glfwSetWindowPos(win_, 0, 0);
    glfwMakeContextCurrent(win_);
    // No vsync: on Wayland a blocked swap (occluded/hidden window) would stall
    // the whole loop and make the compositor flag the app as unresponsive.
    // Frame pacing is done in the main loop instead.
    glfwSwapInterval(0);

    // Resolve GL functions through GLFW (works with GLX and EGL contexts).
    if (imgl3wInit2(reinterpret_cast<GL3WGetProcAddressProc>(glfwGetProcAddress)) != 0) {
        std::fprintf(stderr, "imgl3w: failed to load OpenGL functions\n");
        glfwDestroyWindow(win_);
        glfwTerminate();
        win_ = nullptr;
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    // install_callbacks=true: ImGui installs its own GLFW callbacks, which is
    // what feeds mouse/keyboard events into ImGui. Without this the panel can
    // never receive clicks.
    ImGui_ImplGlfw_InitForOpenGL(win_, true);
    ImGui_ImplOpenGL3_Init("#version 130");

#ifdef GLFW_EXPOSE_NATIVE_X11
    setup_x11();
#endif
    // On X11 we can shape the window click-through, so the panel starts in
    // display-only mode (toggle with 'p'). On Wayland there is no click-through,
    // so the panel is interactive from the start.
    panel_mode_ = glfwGetPlatform() != GLFW_PLATFORM_X11;
    update_clickthrough();  // no-op on Wayland
    glfwShowWindow(win_);

    // Hyprland: pin the window over the game. A special-workspace window
    // floats above everything, including fullscreen games.
    if (hypr_) {
        if (game_w_ <= 0 || game_h_ <= 0)
            hypr::find_game_window(game_x_, game_y_, game_w_, game_h_);
        if (game_w_ > 0 && game_h_ > 0) {
            hypr::place_overlay(game_x_, game_y_, game_w_, game_h_);
            hypr_placed_ = true;
        }
    }
    ok_ = true;
    return true;
}

void ImGuiOverlay::render(const Snapshot& snap, const OverlaySettings& settings) {
    if (!ok_) return;

    // Re-pin every ~5 s so the overlay follows the game across restarts or
    // resolution changes (only when the game geometry actually moved).
    if (hypr_ && (++hypr_tick_ % 300) == 0) {
        int x = 0, y = 0, w = 0, h = 0;
        if (hypr::find_game_window(x, y, w, h) && w > 0 && h > 0) {
            if (x != game_x_ || y != game_y_ || w != game_w_ || h != game_h_) {
                game_x_ = x; game_y_ = y; game_w_ = w; game_h_ = h;
                hypr::place_overlay(x, y, w, h);
            }
        }
    }

    poll_hotkeys();
    glfwPollEvents();
    if (glfwWindowShouldClose(win_)) {
        std::fprintf(stderr, "overlay: window close requested\n");
        close_ = true;
    }

    ImGuiIO& io = ImGui::GetIO();
    if (io.MouseClicked[0] && !mouse_click_logged_) {
        std::fprintf(stderr, "overlay: ImGui click at (%.0f, %.0f)\n",
                     io.MousePos.x, io.MousePos.y);
        mouse_click_logged_ = true;
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    draw_esp(snap, settings);
    draw_panel(snap, settings);

    ImGui::Render();
    glViewport(0, 0, fb_w_, fb_h_);
    glClearColor(0.f, 0.f, 0.f, 0.f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(win_);
}

void ImGuiOverlay::draw_esp(const Snapshot& snap, const OverlaySettings& settings) {
    if (!settings.esp_on || !*settings.esp_on) return;
    if (!snap.valid) return;

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const float sw = ImGui::GetIO().DisplaySize.x;
    const float sh = ImGui::GetIO().DisplaySize.y;

    for (const Player& p : snap.players) {
        if (!p.valid || !p.alive) continue;
        if (!cfg::ESP_SHOW_TEAMMATES && p.team == snap.local.team) continue;
        if (settings.esp_max_distance && p.distance_m > *settings.esp_max_distance) continue;

        Vector2 feet, head;
        if (!WorldToScreen(p.feet, snap.view_matrix, static_cast<int>(sw),
                           static_cast<int>(sh), feet))
            continue;
        if (!WorldToScreen(p.head, snap.view_matrix, static_cast<int>(sw),
                           static_cast<int>(sh), head))
            continue;

        const float box_h = feet.y - head.y;
        if (box_h < 8.f) continue;
        const float box_w = std::max(4.f, box_h * 0.55f);
        const ImVec2 a{feet.x - box_w * 0.5f, head.y};
        const ImVec2 b{feet.x + box_w * 0.5f, feet.y};
        const ImU32 col = team_color(p.team, p.team == snap.local.team);

        dl->AddRect(a, b, col, 0.f, 0, 1.5f);

        // Health bar on the left edge.
        const float bar_h = box_h * std::clamp(p.health, 0, 100) / 100.f;
        dl->AddRectFilled({a.x - 5.f, b.y - bar_h}, {a.x - 3.f, b.y},
                          health_color(p.health));

        // Health + distance label.
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%d HP  %.0fm", p.health, p.distance_m);
        dl->AddText({b.x + 4.f, a.y}, IM_COL32(255, 255, 255, 235), buf);
    }
}

void ImGuiOverlay::draw_panel(const Snapshot& snap, const OverlaySettings& settings) {
    const bool interactive = panel_mode_;
    ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize;
    if (!interactive) flags |= ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing;

    ImGui::SetNextWindowPos({12.f, 12.f}, ImGuiCond_FirstUseEver);
    ImGui::Begin("cs2-cheat", nullptr, flags);

    if (settings.esp_on) ImGui::Checkbox("ESP", settings.esp_on);
    if (settings.aim_on) ImGui::Checkbox("Aimbot", settings.aim_on);
    if (settings.trigger_on) ImGui::Checkbox("Triggerbot", settings.trigger_on);

    ImGui::Separator();
    if (settings.aim_fov_deg)
        ImGui::SliderFloat("FOV (deg)", settings.aim_fov_deg, 1.f, 60.f, "%.1f");
    if (settings.aim_smooth)
        ImGui::SliderFloat("Smooth", settings.aim_smooth, 1.f, 30.f, "%.1f");
    if (settings.esp_max_distance)
        ImGui::SliderFloat("Max dist (m)", settings.esp_max_distance, 10.f, 400.f, "%.0f");

    ImGui::Separator();
    const char* status = !snap.connected ? "cs2 not found"
                        : !snap.valid    ? "connected - in menu"
                                         : "connected - in match";
    ImGui::TextDisabled("game: %s", status);
    int enemies = 0;
    for (const Player& p : snap.players) {
        if (p.valid && p.alive && p.team != snap.local.team) ++enemies;
    }
    ImGui::TextDisabled("enemies: %d", enemies);
    ImGui::TextDisabled("press 'p' to %s panel", interactive ? "close" : "open");

    ImGui::End();
}

void ImGuiOverlay::poll_hotkeys() {
    // GLFW path (window focused: Wayland, or X11 while in panel mode).
    const bool p_down = glfwGetKey(win_, GLFW_KEY_P) == GLFW_PRESS;
    if (p_down && !p_was_down_) panel_mode_ = !panel_mode_;
    p_was_down_ = p_down;

#ifdef GLFW_EXPOSE_NATIVE_X11
    // Global grab path (X11 click-through mode, window never focused).
    if (x11_) {
        Display* dpy = glfwGetX11Display();
        if (dpy) {
            XEvent e;
            while (XCheckWindowEvent(dpy, DefaultRootWindow(dpy), KeyPressMask, &e)) {
                if (XLookupKeysym(&e.xkey, 0) == XK_P) panel_mode_ = !panel_mode_;
            }
        }
    }
#endif
}

void ImGuiOverlay::update_clickthrough() {
#ifdef GLFW_EXPOSE_NATIVE_X11
    if (!x11_) return;
    Display* dpy = glfwGetX11Display();
    if (!dpy) return;
    if (panel_mode_) {
        const XRectangle full = {0, 0, static_cast<unsigned short>(fb_w_),
                                 static_cast<unsigned short>(fb_h_)};
        XShapeCombineRectangles(dpy, x11_win_, ShapeInput, 0, 0, &full, 1,
                                ShapeSet, Unsorted);
    } else {
        XShapeCombineRectangles(dpy, x11_win_, ShapeInput, 0, 0, nullptr, 0,
                                ShapeSet, Unsorted);
    }
    XFlush(dpy);
#else
    (void)0;
#endif
}

#ifdef GLFW_EXPOSE_NATIVE_X11
void ImGuiOverlay::setup_x11() {
    Display* dpy = glfwGetX11Display();
    if (!dpy) return;
    x11_win_ = glfwGetX11Window(win_);
    x11_ = true;

    // Global hotkey for panel toggle (works even while click-through).
    const KeyCode kc = XKeysymToKeycode(dpy, XK_P);
    XGrabKey(dpy, kc, AnyModifier, DefaultRootWindow(dpy), True,
             GrabModeAsync, GrabModeAsync);
    XFlush(dpy);
}
#endif

void ImGuiOverlay::shutdown() {
    if (!ok_) return;
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    if (win_) {
        glfwDestroyWindow(win_);
        win_ = nullptr;
    }
    glfwTerminate();
    ok_ = false;
}
