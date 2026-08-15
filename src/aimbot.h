#pragma once

#include "bvh.h"
#include "game.h"

#include <cstdint>

// Aim assistant: picks the best enemy within a distance-adaptive FOV cone,
// steers the camera and optionally filters targets by BVH line-of-sight.
// Owns the per-map BVH, the locked-target state and the aim-punch filter.
class Aimbot {
public:
    explicit Aimbot(Game& game) : game_(game) {}

    // Loads the map BVH for visibility checks (once; no-op if already loaded).
    // Called periodically from the data thread; safe to call with 0 world ptr.
    // force=true: drop the current map's BVH and reload (map changed).
    bool init_bvh(std::uintptr_t vphys_world, bool force = false);

    // Writes smoothed view angles toward the best enemy within the FOV cone.
    // Returns true if the camera was steered this frame.
    bool run(bool enabled, float fov_deg, float smooth);

    // deadlocked visible(): ray from the local eye to 5 bones; ANY clear bone =
    // visible. Bones: Head(7), LeftFoot(19), RightFoot(22), LeftHand(11),
    // RightHand(15). BVH not loaded -> considered visible.
    bool visible(const Player& target) const;

private:
    bool read_aim_punch(Vector3& out) const;

    Game& game_;
    bvh::MapBvh map_bvh_;
    Vector3 prev_punch_{};
    std::uintptr_t locked_target_ = 0;  // keep until invalid (deadlocked target lock)
};
