#include "math.h"

#include <cmath>
#include <cstdio>

#define CHECK(cond)                                              \
    do {                                                         \
        if (!(cond)) {                                           \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            return 1;                                            \
        }                                                        \
    } while (0)

int main() {
    // CalcAngle: straight ahead along +x => yaw 0, pitch 0.
    Vector3 a = CalcAngle({0, 0, 0}, {100, 0, 0});
    CHECK(std::fabs(a.x) < 0.001f && std::fabs(a.y) < 0.001f);

    // Straight up => pitch -90 (Source: up is negative pitch).
    Vector3 up = CalcAngle({0, 0, 0}, {0, 0, 100});
    CHECK(std::fabs(up.x + 90.f) < 0.001f);

    // Along +y => yaw +90 (positive yaw turns left).
    Vector3 left = CalcAngle({0, 0, 0}, {0, 100, 0});
    CHECK(std::fabs(left.y - 90.f) < 0.001f);

    // ClampAngle wraps yaw.
    Vector3 c{45.f, 190.f, 0.f};
    ClampAngle(c);
    CHECK(std::fabs(c.y + 170.f) < 0.001f);

    // AngleDistance respects the -180/180 wrap.
    float d = AngleDistance({0.f, -170.f, 0.f}, {0.f, 170.f, 0.f});
    CHECK(std::fabs(d - 20.f) < 0.01f);

    // WorldToScreen: a point directly in front of the camera maps to center.
    // Camera looking down -z: w = z, x' = x, y' = y.
    float vm[16]{};
    vm[0] = 1.f;   // x row
    vm[5] = 1.f;   // y row
    vm[10] = 1.f;  // z row
    vm[14] = 1.f;  // w = z
    Vector2 s;
    CHECK(WorldToScreen({0.f, 0.f, 100.f}, vm, 1920, 1080, s));
    CHECK(std::fabs(s.x - 960.f) < 0.5f);
    CHECK(std::fabs(s.y - 540.f) < 0.5f);

    // Off to the right edge.
    CHECK(WorldToScreen({100.f, 0.f, 100.f}, vm, 1920, 1080, s));
    CHECK(std::fabs(s.x - 1920.f) < 0.5f);

    // Behind the camera => rejected.
    CHECK(!WorldToScreen({0.f, 0.f, -1.f}, vm, 1920, 1080, s));

    std::printf("all math tests passed\n");
    return 0;
}
