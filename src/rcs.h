#pragma once

#include "game.h"
#include "memory.h"

#include <deque>

// Recoil Control System - ported from deadlocked cs2/features/rcs.rs.
// Independent of the aimbot: while firing, reads the aim-punch DELTA and moves
// the virtual mouse to compensate, so the spray stays on target even without
// a locked target. Uses acceleration clamping + strength scaling.
class Rcs {
public:
    // Called every frame from the data thread. enabled = menu RCS switch,
    // strength = 0..1 compensation amount from the panel.
    void run(Game& game, const Memory& mem, bool enabled, float strength);

private:
    Vector2 prev_punch_{};
    Vector2 unaccounted_{};
    Vector2 velocity_{};
    std::deque<Vector2> accel_history_;
    int last_shots_ = -1;

    void reset_smoothing() {
        velocity_ = {};
        accel_history_.clear();
    }
};
