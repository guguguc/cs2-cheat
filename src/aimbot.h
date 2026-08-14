#pragma once

#include "game.h"
#include "memory.h"

// Writes smoothed view angles toward the best enemy within the FOV cone.
// Returns true if the camera was steered this frame.
bool run_aimbot(Game& game, const Memory& mem, bool enabled,
                float fov_deg, float smooth);

// Loads the map BVH for visibility checks (once; no-op if already loaded).
// Called periodically from the data thread; safe to call with 0 world ptr.
// force=true: drop the current map's BVH and reload (map changed).
bool aimbot_init_bvh(const Memory& mem, std::uintptr_t vphys_world, bool force = false);
