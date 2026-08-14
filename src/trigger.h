#pragma once

#include "game.h"
#include "memory.h"

#include <chrono>

// Fires through the shared uinput virtual mouse when the crosshair rests on
// an alive, visible enemy (deadlocked triggerbot semantics: random 100-200ms
// delay, fixed shot duration, flash/scope/speed checks). Owns all firing -
// the aimbot only steers.
class Triggerbot {
public:
    void run(Game& game, const Memory& mem, bool enabled, bool& fire_now);
    void run_shoot();

private:
    bool shot_pending_ = false;  // a shot is scheduled (waiting for delay)
    std::chrono::steady_clock::time_point shot_start_{};
    std::chrono::steady_clock::time_point shot_end_{};
    bool firing_ = false;
};
