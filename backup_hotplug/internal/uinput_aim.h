#pragma once

#include <cstdint>

// Aimbot via a virtual mouse (/dev/uinput), like the Linux external cheats do.
// The game reads the virtual device as a real mouse, so view angles are never
// written to memory - sidesteps the runtime-stubbed CreateMove / stale-global
// problems entirely. Works on Wayland (kernel-level device, no X11 needed).
namespace uinput_aim {

bool init();                                   // create the virtual mouse
void set_input_obj(std::uintptr_t input_obj);  // for reading live view angles
void move_to(float target_pitch, float target_yaw);  // steer toward angle
void set_fire(bool on);                            // virtual left-click
void flush();                                      // send a neutral (0,0)+SYN
bool active();                                     // device exists right now
void shutdown();

}  // namespace uinput_aim
