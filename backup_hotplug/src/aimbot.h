#pragma once

#include "game.h"
#include "memory.h"

// Writes smoothed view angles toward the best enemy within the FOV cone.
// Returns true if the camera was steered this frame.
bool run_aimbot(Game& game, const Memory& mem, bool enabled,
                float fov_deg, float smooth);
