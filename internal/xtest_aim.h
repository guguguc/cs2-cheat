#pragma once

#include <cstdint>

// Aimbot via XTEST fake input on the X server the game runs on (XWayland).
// Unlike uinput this creates no pointer device, so there is no hot-plugging,
// no pointer warp on destroy and no conflict with the game's raw-input init.
namespace xtest_aim {

bool init();                                    // open display + XTest ext
void set_input_obj(std::uintptr_t input_obj);   // read live view angles
void move_to(float target_pitch, float target_yaw);
void set_fire(bool on);                         // XTest left button
void shutdown();

}  // namespace xtest_aim
