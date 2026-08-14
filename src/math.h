#pragma once
#include <cmath>

// -----------------------------------------------------------------------------
// Minimal vector / angle math. Header-only and dependency-free so the same file
// compiles on any platform (see tests/math_test.cpp).
// -----------------------------------------------------------------------------

struct Vector2 {
    float x = 0.f;
    float y = 0.f;

    Vector2() = default;
    Vector2(float x, float y) : x(x), y(y) {}

    Vector2 operator+(const Vector2& o) const { return {x + o.x, y + o.y}; }
    Vector2 operator-(const Vector2& o) const { return {x - o.x, y - o.y}; }
};

struct Vector3 {
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;

    Vector3() = default;
    Vector3(float x, float y, float z) : x(x), y(y), z(z) {}

    Vector3 operator+(const Vector3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vector3 operator-(const Vector3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vector3 operator*(float s) const { return {x * s, y * s, z * s}; }
    Vector3 operator*(const Vector3& o) const { return {x * o.x, y * o.y, z * o.z}; }
    Vector3 operator/(float s) const { return {x / s, y / s, z / s}; }

    float& operator[](int i) { return i == 0 ? x : (i == 1 ? y : z); }
    float operator[](int i) const { return i == 0 ? x : (i == 1 ? y : z); }
    float Length() const { return std::sqrt(x * x + y * y + z * z); }
    float Length2D() const { return std::sqrt(x * x + y * y); }
};

inline constexpr float kPi       = 3.14159265358979323846f;
inline constexpr float kRad2Deg  = 180.0f / kPi;
inline constexpr float kDeg2Rad  = kPi / 180.0f;

// Pitch/yaw needed to look from `src` at `dst`, in degrees.
// Result matches the in-game QAngle layout: x = pitch (up = negative),
// y = yaw (0 = +x, positive turns left), z = roll (unused).
inline Vector3 CalcAngle(const Vector3& src, const Vector3& dst) {
    const Vector3 d = dst - src;
    if (d.Length() < 0.0001f) return {0.f, 0.f, 0.f};
    const float pitch = -std::asin(d.z / d.Length()) * kRad2Deg;
    const float yaw   = d.Length2D() < 0.0001f
                            ? 0.f
                            : std::atan2(d.y, d.x) * kRad2Deg;
    return {pitch, yaw, 0.f};
}

inline void ClampAngle(Vector3& a) {
    if (a.x > 89.f) a.x = 89.f;
    if (a.x < -89.f) a.x = -89.f;
    while (a.y > 180.f) a.y -= 360.f;
    while (a.y < -180.f) a.y += 360.f;
    a.z = 0.f;
}

// Shortest angular distance (degrees) between two angle vectors.
inline float AngleDistance(const Vector3& a, const Vector3& b) {
    Vector3 d = a - b;
    ClampAngle(d);
    return std::sqrt(d.x * d.x + d.y * d.y);
}

// View-matrix world-to-screen projection. `vm` is the 4x4 from
// libclient.so + dwViewMatrix, row-major. Returns false when behind camera.
inline bool WorldToScreen(const Vector3& world, const float vm[16],
                          int sw, int sh, Vector2& out) {
    const float w = vm[12] * world.x + vm[13] * world.y + vm[14] * world.z + vm[15];
    if (w < 0.001f) return false;
    const float x = vm[0] * world.x + vm[1] * world.y + vm[2] * world.z + vm[3];
    const float y = vm[4] * world.x + vm[5] * world.y + vm[6] * world.z + vm[7];
    const float inv = 1.0f / w;
    out.x = (static_cast<float>(sw) * 0.5f) * (1.0f + x * inv);
    out.y = (static_cast<float>(sh) * 0.5f) * (1.0f - y * inv);
    return true;
}

// Source engine units are inches.
inline constexpr float kUnitsPerMeter = 39.3701f;
