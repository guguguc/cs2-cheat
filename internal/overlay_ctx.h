#pragma once

#include "game.h"

#include <mutex>

// Shared state between the background game-data thread and the Vulkan present
// hook (render thread).
struct SharedCtx {
    std::mutex mtx;
    Snapshot snap;
    bool valid = false;

    // settings (toggled from the in-game panel)
    bool esp_on = true;
    bool head_circle = true;  // draw a circle around the head (deadlocked style)
    bool aim_on = true;
    bool visibility_check = true;  // aimbot only aims at LOS-visible targets
    bool aim_toggle = false;  // X key toggles aimbot on/off
    bool aim_active = false;  // aimbot is currently steering toward a target
    Vector3 aim_target_world{};  // locked target's head (world); used by the hint box
    bool trigger_on = false;
    bool trigger_head_only = true;  // false = fire on any body part
    // aimbot active while the menu checkbox is checked
    float aim_fov = 12.f;
    float aim_smooth = 6.f;
    float esp_max_dist = 200.f;
    bool panel_open = false;  // menu closed by default; F1 toggles it
    int ui_tab = 0;           // left nav: 0=AIM 1=ESP 2=TRIGGER 3=SETTINGS
};

extern SharedCtx g_ctx;
