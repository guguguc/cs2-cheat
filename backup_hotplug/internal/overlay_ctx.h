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
    bool aim_on = true;
    bool trigger_on = false;
    bool aim_hold = false;   // aimbot steers only while the aim key is held
    float aim_fov = 12.f;
    float aim_smooth = 6.f;
    float esp_max_dist = 200.f;
    bool panel_open = false;  // menu closed by default; F1 toggles it
};

extern SharedCtx g_ctx;
