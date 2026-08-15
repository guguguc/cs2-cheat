#include "aimbot.h"

#include "cfg.h"
#include "config.h"
#include "math.h"
#include "overlay_ctx.h"

#include <algorithm>
#include <cmath>

namespace {

// ---------------------------------------------------------------------------
// Recoil compensation, ported from deadlocked (cs2/features/aimbot.rs +
// cs2/entity/player.rs aim_punch + find_offsets.rs).
//
// The game stores the current aim punch as the LAST element of a CUtlVector
// (m_aimPunchCache) on CCSPlayer_AimPunchServices. The vector sits right
// before the first network var:
//     aim_punch_cache = CCSPlayer_AimPunchServices::m_unpredictableBaseTick
//                       - 0x18
// Layout (verified live): +0x00 = count (i32), +0x08 = data ptr, elements are
// 12-byte QAngle (pitch, yaw, roll).
//
// The engine stores HALF the real punch, so RCS multiplies by 2. The aimbot
// then aims at `target_angle - punch*2` so the recoil-raised crosshair still
// lands on the target. Mid-spray the punch can read as zero; we then keep the
// previous punch (shots_fired > 1), exactly like deadlocked.
// ---------------------------------------------------------------------------

// deadlocked distance_scale: far targets get a tight FOV, close ones wider.
float distance_scale(float distance) {
    if (distance > 500.f) return 1.0f;
    return 5.0f - distance / 125.0f;
}

}  // namespace

bool Aimbot::read_aim_punch(Vector3& out) const {
    const std::uintptr_t pawn = game_.local_pawn();
    if (!pawn) return false;
    const auto services = game_.memory().read<std::uintptr_t>(
        pawn + Config::instance().offsets.m_pAimPunchServices);
    if (!services || !*services) return false;
    const auto len = game_.memory().read<std::int32_t>(
        *services + Config::instance().offsets.aimPunchCache);
    if (!len || *len < 1) return false;
    const auto data = game_.memory().read<std::uintptr_t>(
        *services + Config::instance().offsets.aimPunchCache + 0x08);
    if (!data || !*data) return false;
    const auto punch = game_.memory().read<Vector3>(*data + (*len - 1) * 12);
    if (!punch) return false;
    out = *punch;
    return true;
}

bool Aimbot::visible(const Player& target) const {
    if (map_bvh_.triangle_count() == 0) return true;  // BVH not loaded
    const Vector3 eye = game_.local_eye();
    const int bones[5] = {7, 19, 22, 11, 15};
    for (const int b : bones) {
        Vector3 pos{};
        if (!game_.read_bone(target.address, b, pos)) continue;
        if (map_bvh_.has_line_of_sight(eye, pos)) return true;
    }
    return false;
}

bool Aimbot::init_bvh(std::uintptr_t vphys_world, bool force) {
    if (!vphys_world) return false;
    if (force) map_bvh_.reset();  // map changed: drop old geometry
    if (!force && map_bvh_.triangle_count() != 0) return true;
    return map_bvh_.load(game_.memory(), vphys_world);
}

bool Aimbot::run(bool enabled, float fov_deg, float smooth) {
    (void)smooth;
    if (!enabled || !game_.attached() || !game_.local_pawn()) return false;

    const Player& local = game_.local();
    if (!local.alive) return false;
    const Vector3 eye = game_.local_eye();

    // Target selection (deadlocked): pick the enemy with the smallest angle
    // to the crosshair, within a distance-adaptive FOV cone. Re-evaluated
    // every frame, so moving the crosshair onto another enemy switches to it.
    const Player* best = nullptr;
    {
        float best_angle = fov_deg * distance_scale(1.f);
        for (const auto& p : game_.players()) {
            if (!p.valid || !p.alive || p.local) continue;
            if (p.team == local.team) continue;
            if (p.distance_m > cfg::AIM_MAX_DISTANCE_M) continue;
            if (g_ctx.visibility_check && !visible(p))
                continue;  // optional BVH line-of-sight filter (menu toggle)
            const Vector3 target = CalcAngle(eye, p.head);
            const float dist = AngleDistance(target, game_.view_angles());
            const float limit = fov_deg * distance_scale(p.distance_m);
            if (dist <= limit && dist < best_angle) {
                best_angle = dist;
                best = &p;
            }
        }
    }
    // Target switch: reset recoil state so the previous weapon's punch does
    // not leak onto the new target (deadlocked resets with the target).
    if (!best || (locked_target_ && best->address != locked_target_)) {
        prev_punch_ = {};
    }
    if (!best) {
        locked_target_ = 0;
        return false;
    }
    locked_target_ = best->address;
    {
        std::lock_guard<std::mutex> lk(g_ctx.mtx);
        g_ctx.aim_target_world = best->head;  // hint box follows this target
    }

    // Recoil compensation is handled by the independent RCS module (src/rcs.cpp)
    // exactly like deadlocked - the aimbot only steers toward the head.
    const Vector3 target_angle = CalcAngle(eye, best->head);
    Vector3 comp = target_angle;
    ClampAngle(comp);
    game_.set_view_angles(comp);
    return true;
}
