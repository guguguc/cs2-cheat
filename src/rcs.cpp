#include "rcs.h"

#include "cfg.h"
#include "config.h"
#include "math.h"
#include "uinput_aim.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

// ---------------------------------------------------------------------------
// Recoil Control System - deadlocked cs2/features/rcs.rs port.
//
// While firing (shots_fired >= 1), reads the aim-punch DELTA each frame and
// moves the virtual mouse by an acceleration-clamped amount, scaled by
// `strength`. Runs independently of the aimbot, so spray stays controlled
// even without a locked target. Skips knife/grenade/pistol/shotgun/sniper.
// ---------------------------------------------------------------------------

namespace {

struct AccelTuning {
    float multiplier;
    float range_min, range_max;
    float fallback;
    float decay;
};

constexpr AccelTuning kPitchTuning{3.0f, 4.0f, 20.0f, 10.0f, 0.15f};
constexpr AccelTuning kYawTuning{2.5f, 1.5f, 8.0f, 5.0f, 0.30f};
constexpr float kTrackScaleX = 0.65f;
constexpr float kTrackScaleY = 0.55f;
constexpr std::size_t kAccelHistoryMax = 12;

float weighted_avg_component(const std::deque<Vector2>& h, bool use_x) {
    if (h.empty()) return 0.f;
    float sum = 0.f, wsum = 0.f;
    std::size_t i = 0;
    for (const auto& v : h) {
        const float weight = 1.0f + static_cast<float>(i) * 0.15f;
        sum += (use_x ? v.x : v.y) * weight;
        wsum += weight;
        ++i;
    }
    return sum / wsum;
}

float compute_max_accel(const std::deque<Vector2>& h, bool use_x,
                        const AccelTuning& t) {
    if (h.size() < 3) return t.fallback;
    const float wa = weighted_avg_component(h, use_x) * t.multiplier;
    return std::clamp(wa, t.range_min, t.range_max);
}

float soft_clamp(float accel, float max_accel, float decay) {
    if (std::fabs(accel) <= max_accel) return accel;
    const float excess = std::fabs(accel) - max_accel;
    const float sign = accel >= 0.f ? 1.f : -1.f;
    return sign * (max_accel + excess * std::exp(-excess * decay));
}

void record_accel(std::deque<Vector2>& h, const Vector2& v) {
    if (std::fabs(v.x) < 25.f && std::fabs(v.y) < 25.f) {
        h.push_front({std::fabs(v.x), std::fabs(v.y)});
        if (h.size() > kAccelHistoryMax) h.pop_back();
    }
}

}  // namespace

void Rcs::run(Game& game, const Memory& mem, bool enabled, float strength) {
    if (!enabled || !game.attached() || !game.local_pawn()) return;
    const Player& local = game.local();
    if (!local.alive) return;

    // Weapon filter (deadlocked): skip Unknown/Knife/Grenade/Pistol/Shotgun.
    const std::string wname = game.weapon_name(mem);
    if (wname.empty()) return;
    if (wname == "knife" || wname == "knife_t" || wname == "bayonet" ||
        wname.rfind("knife_", 0) == 0 || wname.find("grenade") != std::string::npos ||
        wname == "flashbang" || wname == "smokegrenade" || wname == "molotov" ||
        wname == "decoy" || wname == "incgrenade" || wname == "taser" ||
        wname == "cz75a" || wname == "deagle" || wname == "elite" ||
        wname == "fiveseven" || wname == "glock" || wname == "hkp2000" ||
        wname == "p250" || wname == "revolver" || wname == "tec9" ||
        wname == "usp_silencer" || wname == "usp_silencer_off" ||
        wname == "mag7" || wname == "nova" || wname == "sawedoff" ||
        wname == "xm1014")
        return;

    // Aim punch (read_aim_punch equivalent): pawn -> services -> cache -> last.
    Vector3 punch{};
    const std::uintptr_t pawn = game.local_pawn();
    if (!pawn) return;
    const auto services = mem.read<std::uintptr_t>(pawn + cs2cfg().offsets.m_pAimPunchServices);
    if (!services || !*services) return;
    const auto len = mem.read<std::int32_t>(*services + cs2cfg().offsets.aimPunchCache);
    if (!len || *len < 1) return;
    const auto data =
        mem.read<std::uintptr_t>(*services + cs2cfg().offsets.aimPunchCache + 0x08);
    if (!data || !*data) return;
    const auto p3 = mem.read<Vector3>(*data + (*len - 1) * 12);
    if (!p3) return;
    punch = *p3;

    const int shots =
        mem.read<int>(pawn + cs2cfg().offsets.m_iShotsFired).value_or(0);

    // deadlocked: sniper -> no punch; zero punch mid-spray -> keep previous.
    Vector2 aim_punch{};
    if (wname == "awp" || wname == "g3sg1" || wname == "scar20" || wname == "ssg08") {
        aim_punch = {};
    } else if (punch.x == 0.f && punch.y == 0.f && shots > 1) {
        aim_punch = prev_punch_;
    } else {
        aim_punch = {punch.x, punch.y};
    }

    if (shots < 1) {
        prev_punch_ = aim_punch;
        unaccounted_ = {};
        reset_smoothing();
        return;
    }

    const float sens = uinput_aim::live_sensitivity();
    if (sens <= 0.01f) return;

    // mouse_angle: punch DELTA converted to mouse counts (deadlocked formula).
    const Vector2 mouse_angle{
        (aim_punch.y - prev_punch_.y) / sens * 100.0f,
        -(aim_punch.x - prev_punch_.x) / sens * 100.0f,
    };
    prev_punch_ = aim_punch;

    strength = std::clamp(strength, 0.f, 1.f);
    const Vector2 desired{
        mouse_angle.x * strength + unaccounted_.x,
        mouse_angle.y * strength + unaccounted_.y,
    };

    const Vector2 raw_accel{desired.x - velocity_.x, desired.y - velocity_.y};
    const Vector2 track{raw_accel.x * kTrackScaleX, raw_accel.y * kTrackScaleY};

    const float max_x = compute_max_accel(accel_history_, true, kPitchTuning);
    const float max_y = compute_max_accel(accel_history_, false, kYawTuning);
    const Vector2 clamped{
        soft_clamp(track.x, max_x, kPitchTuning.decay),
        soft_clamp(track.y, max_y, kYawTuning.decay),
    };
    velocity_.x += clamped.x;
    velocity_.y += clamped.y;
    record_accel(accel_history_, clamped);

    const Vector2 ready{std::trunc(velocity_.x), std::trunc(velocity_.y)};
    unaccounted_ = {desired.x - ready.x, desired.y - ready.y};

    const int dx = static_cast<int>(ready.x);
    const int dy = static_cast<int>(ready.y);
    if (dx != 0 || dy != 0) uinput_aim::move_counts_raw(dx, dy);
}
