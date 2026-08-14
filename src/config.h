#pragma once

namespace cfg {

// ---- terminal hotkeys -------------------------------------------------------
inline constexpr char KEY_ESP     = 'e';
inline constexpr char KEY_AIM     = 'a';
inline constexpr char KEY_TRIGGER = 't';
inline constexpr char KEY_QUIT    = 'q';

// ---- ESP --------------------------------------------------------------------
inline constexpr bool   ESP_SHOW_TEAMMATES    = false;
inline constexpr double RADAR_METERS_PER_CELL = 2.0;   // meters per radar char
inline constexpr int    RADAR_CELLS           = 18;    // +-18 cells => +-36 m
inline constexpr bool   LIST_VIEW             = true;  // text list next to the radar

// ---- aimbot -----------------------------------------------------------------
inline constexpr double AIM_FOV_DEG        = 30.0;    // max angle from crosshair
inline constexpr double AIM_SMOOTH         = 6.0;     // 1.0 = instant snap
inline constexpr double AIM_MAX_DISTANCE_M = 200.0;
inline constexpr int    AIM_BONE           = 6;       // head

// ---- triggerbot (deadlocked semantics) -------------------------------------
inline constexpr bool   TRIGGER_ENABLED_DEFAULT = false;
inline constexpr int    TRIGGER_DELAY_MS_MIN    = 30;   // random shot delay range
inline constexpr int    TRIGGER_DELAY_MS_MAX    = 80;
inline constexpr int    TRIGGER_SHOT_DURATION_MS = 80;  // button hold time
inline constexpr float  TRIGGER_VELOCITY_THRESHOLD = 100.0f;  // moving too fast = no fire
inline constexpr bool   TRIGGER_HEAD_ONLY       = true;  // crosshair must be on head
inline constexpr bool   TRIGGER_FLASH_CHECK     = true;  // no fire while flashed
inline constexpr bool   TRIGGER_SCOPE_CHECK     = true;  // snipers must be scoped

// ---- frame pacing -----------------------------------------------------------
inline constexpr int FRAME_INTERVAL_MS = 16;  // ~60 fps

}  // namespace cfg
