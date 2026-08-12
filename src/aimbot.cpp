#include "aimbot.h"

#include "config.h"
#include "math.h"

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

    // Full aimbot: pass the exact target angle; the uinput driver converts it
    // to mouse movement (proportional per-frame) and auto-fires when locked.
    const Vector3 target_angle = CalcAngle(eye, best->head);
    game.set_view_angles(mem, target_angle);
    return true;
}
