#pragma once

#include "game.h"

struct GLFWwindow;  // opaque; only pointers are stored here

// Runtime settings shared with the ImGui control panel (bound directly to the
// same variables main.cpp uses for the cheat loop).
struct OverlaySettings {
    bool* esp_on = nullptr;
    bool* aim_on = nullptr;
    bool* trigger_on = nullptr;
    float* aim_fov_deg = nullptr;
    float* aim_smooth = nullptr;
    float* esp_max_distance = nullptr;
};

// ImGui + GLFW + OpenGL3 overlay. Renders ESP boxes/health and an interactive
// control panel. Works on X11 and Wayland (GLFW's backends); on Wayland the
// window cannot be click-through or always-on-top, so prefer the terminal
// radar there for guaranteed ESP.
class ImGuiOverlay {
public:
    // `platform` forces the GLFW backend: "x11", "wayland", or "" (auto).
    bool init(const char* platform = "");
    void render(const Snapshot& snap, const OverlaySettings& settings);
    void shutdown();

    bool ok() const { return ok_; }
    bool should_close() const { return close_; }

private:
    void draw_esp(const Snapshot& snap, const OverlaySettings& settings);
    void draw_panel(const Snapshot& snap, const OverlaySettings& settings);
    void poll_hotkeys();
    void update_clickthrough();
#ifdef GLFW_EXPOSE_NATIVE_X11
    void setup_x11();
    bool x11_ = false;
    unsigned long x11_win_ = 0;
#endif

    GLFWwindow* win_ = nullptr;
    bool ok_ = false;
    bool close_ = false;
    bool panel_mode_ = false;
    bool p_was_down_ = false;
    bool mouse_click_logged_ = false;
    int fb_w_ = 0;
    int fb_h_ = 0;

    // Hyprland pinning (overlay above the fullscreen game).
    bool hypr_ = false;
    bool hypr_placed_ = false;
    int hypr_tick_ = 0;
    int game_x_ = 0, game_y_ = 0, game_w_ = 0, game_h_ = 0;
};
