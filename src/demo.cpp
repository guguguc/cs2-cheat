#include "demo.h"

#include "math.h"

#include <cmath>

namespace {

// Build a row-major OpenGL-style view-projection matrix (same convention as
// the game's dwViewMatrix) so the demo exercises the real WorldToScreen path.
void make_view_proj(const Vector3& eye, const Vector3& fwd, const Vector3& up,
                    float fovy_deg, float aspect, float znear, float zfar,
                    float out[16]) {
    Vector3 f = fwd / fwd.Length();
    Vector3 r = {up.y * f.z - up.z * f.y,
                 up.z * f.x - up.x * f.z,
                 up.x * f.y - up.y * f.x};
    r = r / r.Length();
    Vector3 u = {f.y * r.z - f.z * r.y,
                 f.z * r.x - f.x * r.z,
                 f.x * r.y - f.y * r.x};

    // View matrix (rows), right-handed: camera looks down -z.
    float V[16]{
        r.x, r.y, r.z, -(r.x * eye.x + r.y * eye.y + r.z * eye.z),
        u.x, u.y, u.z, -(u.x * eye.x + u.y * eye.y + u.z * eye.z),
        -f.x, -f.y, -f.z, (f.x * eye.x + f.y * eye.y + f.z * eye.z),
        0.f, 0.f, 0.f, 1.f,
    };

    // Perspective projection matrix (rows).
    const float fov = fovy_deg * kDeg2Rad;
    const float f_ = 1.f / std::tan(fov * 0.5f);
    float P[16]{};
    P[0] = f_ / aspect;
    P[5] = f_;
    P[10] = (zfar + znear) / (znear - zfar);
    P[11] = (2.f * zfar * znear) / (znear - zfar);
    P[14] = -1.f;

    // M = P * V.
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            float acc = 0.f;
            for (int k = 0; k < 4; ++k) acc += P[i * 4 + k] * V[k * 4 + j];
            out[i * 4 + j] = acc;
        }
    }
}

}  // namespace

Snapshot make_demo_snapshot(double t) {
    Snapshot s;

    Player local;
    local.local = true;
    local.valid = true;
    local.alive = true;
    local.health = 100;
    local.armor = 50;
    local.team = 3;  // CT
    local.feet = {0.f, 0.f, 0.f};
    local.head = {0.f, 0.f, 72.f};
    s.local = local;

    // Slowly sweeping view (yaw in degrees).
    s.view_angles = {0.f, static_cast<float>(30.0 * std::sin(t * 0.35)), 0.f};

    // A few enemies orbiting at different radii / heights / teams.
    const int n = 6;
    for (int i = 0; i < n; ++i) {
        const double ang = t * 0.4 + i * (2.0 * kPi / n);
        const double radius_m = 6.0 + 4.0 * (i % 3);

        Player p;
        p.valid = true;
        p.alive = true;
        p.team = (i % 2 == 0) ? 2 : 3;  // alternate T / CT
        p.health = 20 + ((i * 17) % 81);
        p.armor = (i * 13) % 101;
        p.flash_alpha = (i == 2) ? 180.f : 0.f;
        p.scoped = (i == 1);
        p.feet = {
            static_cast<float>(radius_m * std::cos(ang)) * kUnitsPerMeter,
            static_cast<float>(radius_m * std::sin(ang)) * kUnitsPerMeter,
            0.f,
        };
        p.head = p.feet + Vector3{0.f, 0.f, 72.f};
        p.velocity = {0.f, 0.f, 0.f};

        // Same radar projection as Game::project_radar.
        const Vector3 rel = (p.feet - local.feet) / kUnitsPerMeter;
        const float yaw = s.view_angles.y * kDeg2Rad;
        const float fwd_x = std::cos(yaw), fwd_y = std::sin(yaw);
        const float right_x = -fwd_y, right_y = fwd_x;
        p.radar_xy.x = rel.x * right_x + rel.y * right_y;
        p.radar_xy.y = rel.x * fwd_x + rel.y * fwd_y;
        p.distance_m = rel.Length();

        s.players.push_back(p);
    }

    // Real view-projection from the (sweeping) view yaw, so the overlay ESP
    // path projects the orbiting enemies onto the screen.
    const float yaw = s.view_angles.y * kDeg2Rad;
    const Vector3 fwd{std::cos(yaw), std::sin(yaw), 0.f};
    make_view_proj({0.f, 0.f, 72.f}, fwd, {0.f, 0.f, 1.f},
                   90.f, 1280.f / 800.f, 1.f, 5000.f, s.view_matrix);
    s.connected = true;
    s.valid = true;
    return s;
}
