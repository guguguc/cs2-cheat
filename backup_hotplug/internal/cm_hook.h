#pragma once

#include <cstdint>

// Input::CreateMove hook.
//
// The game reads its camera view angles from the input object every tick,
// right after CreateMove runs (and after the game copies the angles from the
// current user command into the input object's view-angle slots). External /
// thread writes miss that tiny window, which is why writing the slots from a
// separate thread never steered the camera. This hook runs INSIDE CreateMove,
// so angles we write here are picked up by the camera this very tick.
//
// Layout (native Linux CS2, build 8.7, resolved empirically):
//   input_obj     = *(uintptr_t*)(client + 0x4576E98)      (dwCSGOInput)
//   view-angle    = input_obj + 0x9C / +0x548 / +0x5F0     (QAngle mirrors)
//   CreateMove    = (*(uintptr_t**)input_obj)[8]           (vtable slot 8)
namespace cm_hook {

// Patch the input object's vtable slot 8 to our CreateMove hook.
// `input_obj` is the live CCSGOInput object pointer (client+dwCSGOInput).
bool install(std::uintptr_t input_obj);

// Remove the hook (restore the original vtable slot).
void uninstall();

// Steer the camera: the hook writes these angles into the input object's
// view-angle slots on every CreateMove while `active` is true.
void set_cmd_angles(float pitch, float yaw, bool active);

// Verification: for the next `seconds` seconds the hook forces the camera to
// (0, +90, 0) regardless of set_cmd_angles. Used once to prove the hook works.
void steer_test(float seconds);

std::uintptr_t input_obj();
std::uint64_t calls();           // how many times the hook fired
std::uint64_t orig_fn();         // saved original vtable slot
bool active();

}  // namespace cm_hook
