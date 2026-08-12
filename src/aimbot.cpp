#include "aimbot.h"

#include "cfg.h"
#include "config.h"
#include "math.h"

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
bool read_aim_punch(const Game& game, const Memory& mem, Vector3& out) {
    const std::uintptr_t pawn = game.local_pawn();
    if (!pawn) return false;
    const auto services =
        mem.read<std::uintptr_t>(pawn + cs2cfg().offsets.m_pAimPunchServices);
    if (!services || !*services) return false;
    const auto len =
        mem.read<std::int32_t>(*services + cs2cfg().offsets.aimPunchCache);
    if (!len || *len < 1) return false;
    const auto data =
        mem.read<std::uintptr_t>(*services + cs2cfg().offsets.aimPunchCache + 0x08);
    if (!data || !*data) return false;
    const auto punch = mem.read<Vector3>(*data + (*len - 1) * 12);
    if (!punch) return false;
    out = *punch;
    return true;
}

Vector3 g_prev_punch{};

}  // namespace

bool run_aimbot(Game& game, const Memory& mem, bool enabled,
                float fov_deg, float smooth) {
    (void)smooth;
    if (!enabled || !game.attached() || !game.local_pawn()) return false;

    const Player& local = game.local();
    if (!local.alive) return false;
    const Vector3 eye = game.local_eye(mem);

    const Player* best = nullptr;
    float best_angle = fov_deg;
    for (const auto& p : game.players()) {
        if (!p.valid || !p.alive || p.local) continue;
        if (p.team == local.team) continue;
        if (p.distance_m > cfg::AIM_MAX_DISTANCE_M) continue;

        const Vector3 target = CalcAngle(eye, p.head);
        const float dist = AngleDistance(target, game.view_angles());
        if (dist < best_angle) {
            best_angle = dist;
            best = &p;
        }
    }
    if (!best) return false;

    // --- recoil compensation (deadlocked) ---
    Vector3 punch{};
    const bool have_punch = read_aim_punch(game, mem, punch);
    const int shots =
        mem.read<int>(game.local_pawn() + cs2cfg().offsets.m_iShotsFired).value_or(0);
    Vector3 eff = punch * 2.0f;  // engine stores half the real punch
    if (have_punch && eff.Length() == 0.f && shots > 1) {
        // Punch cleared mid-spray: keep compensating with the last known value.
        eff = g_prev_punch;
    }
    g_prev_punch = eff;

    // Aim at `target - punch*2` so the recoil-raised crosshair hits the head.
    const Vector3 target_angle = CalcAngle(eye, best->head);
    Vector3 comp = target_angle;
    comp.x -= eff.x;
    comp.y -= eff.y;
    ClampAngle(comp);
    game.set_view_angles(mem, comp);
    return true;
}
