#pragma once

struct ImGuiIO;

// Global X11 input for the in-game panel. Opens a second connection to the X
// server from inside the game process, finds the game's own window (by
// _NET_WM_PID), polls the pointer and feeds ImGui. 'p' toggles the panel.
namespace input_x11 {

bool init();
void poll(ImGuiIO& io);
// True when the XInput2 raw-motion path is active (cursor follows the mouse
// even when the game freezes the OS cursor).
bool raw_tracking();

}  // namespace input_x11
