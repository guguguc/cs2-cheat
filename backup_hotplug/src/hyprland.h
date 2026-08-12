#pragma once

// Minimal Hyprland IPC helpers used to pin the overlay window directly above
// the fullscreen game. Hyprland renders special-workspace ("scratchpad")
// windows above every other window — including fullscreen games — which is
// exactly what an external overlay needs.
namespace hypr {

// True when running under Hyprland (or when `hyprctl` responds).
bool available();

// Geometry of the game window (Hyprland class "cs2"). Returns false when the
// game is not running / not found.
bool find_game_window(int& x, int& y, int& w, int& h);

// Moves the overlay window (matched by title) to the "cs2cheat" special
// workspace, floats it, and pins it at (x, y) sized (w, h).
bool place_overlay(int x, int y, int w, int h);

}  // namespace hypr
