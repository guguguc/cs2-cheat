#pragma once

#include "game.h"

// Synthetic game feed so the radar / aim math can be exercised without CS2
// running: `cs2_cheat --demo`.
Snapshot make_demo_snapshot(double t_seconds);
