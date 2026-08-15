#include "trigger.h"

#include "cfg.h"
#include "config.h"
#include "mouse_device.h"
#include "overlay_ctx.h"

#include <cmath>
#include <cstdio>
#include <random>

// deadlocked triggerbot (cs2/features/triggerbot.rs):
//   - enabled via menu + hotkey (mode Toggle)
//   - checks: flash, scope (snipers must be scoped), velocity, same-team
//   - head_only: crosshair must be within a distance-scaled radius of the
//     enemy's head bone before firing
//   - fires with a random 100-200ms delay (normal distribution), holds the
//     button for `shot_duration` ms, via the shared uinput virtual mouse.
void Triggerbot::run(bool enabled, bool& fire_now) {
    fire_now = false;
    if (!enabled || !game_.attached() || !game_.local_pawn()) {
        // Force-release if the bot was mid-shot when disabled.
        if (firing_) { g_mouse.set_fire(false); firing_ = false; }
        shot_pending_ = false;
        shot_start_ = {};
        return;
    }

    const auto now = std::chrono::steady_clock::now();

    // Shot in progress: drive the button release.
    if (firing_) {
        if (now >= shot_end_) {
            g_mouse.set_fire(false);
            firing_ = false;
        }
        return;
    }
    if (shot_pending_) return;  // already scheduled; run_shoot() fires it

    const Player& local = game_.local();
    if (!local.alive) return;

    // --- flash check (deadlocked: is_flashed > 0.2) ---
    const auto flash = game_.memory().read<float>(
        game_.local_pawn() + Config::instance().offsets.m_flFlashOverlayAlpha);
    if (flash && *flash > 0.2f) return;

    // --- velocity check (deadlocked default threshold 100) ---
    const auto vel = game_.memory().read<Vector3>(
        game_.local_pawn() + Config::instance().offsets.m_vecVelocity);
    if (vel && vel->Length() > cfg::TRIGGER_VELOCITY_THRESHOLD) return;

    // --- crosshair entity (m_iIDEntIndex) ---
    const auto idx = game_.memory().read<int>(
        game_.local_pawn() + Config::instance().offsets.m_iIDEntIndex);
    if (!idx || *idx <= 0) return;
    const std::uintptr_t ent = game_.entity_by_index(*idx);
    if (!ent || ent == game_.local_pawn()) return;
    if (game_.entity_team(ent) == local.team) return;  // no friendlies
    const int hp = game_.entity_health(ent);
    if (hp <= 0 || hp > 100) return;

    // --- head_only: crosshair must be near the head bone (distance-scaled).
    // Menu toggle; off = fire on any body part (deadlocked optional).
    bool head_only = true;
    {
        std::lock_guard<std::mutex> lk(g_ctx.mtx);
        head_only = g_ctx.trigger_head_only;
    }
    if (head_only) {
        Vector3 head{};
        if (!game_.read_bone(ent, Config::instance().offsets.boneHeadIndex, head)) return;
        const Vector3 eye = game_.local_eye();
        const float dist = (head - eye).Length();
        if (dist < 1.f) return;
        // angle between view and head direction
        const Vector3 dir = (head - eye) / dist;
        const Vector3 vfwd = game_.view_forward();
        const float dot = vfwd.x * dir.x + vfwd.y * dir.y + vfwd.z * dir.z;
        const float fov = std::acos(std::clamp(dot, -1.f, 1.f)) * 57.29578f;
        const float head_radius_fov = 3.5f / dist * 100.0f;
        if (fov > head_radius_fov) return;
    }

    // --- schedule a shot: random 100-200ms delay, then hold shot_duration ---
    static std::mt19937 rng{std::random_device{}()};
    std::normal_distribution<float> dist((cfg::TRIGGER_DELAY_MS_MIN + cfg::TRIGGER_DELAY_MS_MAX) / 2.0f,
                                         (cfg::TRIGGER_DELAY_MS_MAX - cfg::TRIGGER_DELAY_MS_MIN) / 2.0f);
    const auto delay_ms = static_cast<long long>(std::max(0.0f, dist(rng)));
    shot_pending_ = true;
    shot_start_ = now + std::chrono::milliseconds(delay_ms);
    shot_end_ = shot_start_ + std::chrono::milliseconds(cfg::TRIGGER_SHOT_DURATION_MS);
    (void)fire_now;
}

// Called every frame from the data thread; drives the delayed shot.
void Triggerbot::run_shoot() {
    const auto now = std::chrono::steady_clock::now();
    if (firing_) return;
    if (!shot_pending_) return;
    if (now >= shot_start_) {
        g_mouse.set_fire(true);
        firing_ = true;
        shot_pending_ = false;
        shot_start_ = {};
    }
}
