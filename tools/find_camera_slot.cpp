// find_camera_slot - brute-force locate the slot that actually steers the CS2
// camera, by scanning ALL writable process memory for QAngle triples that
// match the current camera angle, then continuous-write-testing each one.
//
// The camera angles come from the input system's current user command; that
// slot holds the live (pitch, yaw, roll) and is updated every tick, so it
// must match the view-matrix-derived angle at scan time.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "memory.h"
#include "process.h"

namespace {

constexpr std::uintptr_t kViewMatrixOff = 0x4809800;
constexpr double kPi = 3.14159265358979323846;
constexpr float kRad2Deg = 180.f / static_cast<float>(kPi);

struct Range {
    std::uintptr_t start, end;
};

std::vector<Range> writable_ranges(int pid) {
    std::vector<Range> out;
    std::ifstream maps("/proc/" + std::to_string(pid) + "/maps");
    std::string line;
    while (std::getline(maps, line)) {
        const auto dash = line.find('-');
        const auto spc = line.find(' ');
        if (dash == std::string::npos || spc == std::string::npos) continue;
        const std::uintptr_t start =
            std::stoull(line.substr(0, dash), nullptr, 16);
        const std::uintptr_t end =
            std::stoull(line.substr(dash + 1, spc - dash - 1), nullptr, 16);
        const std::string perms = line.substr(spc + 1, 4);
        // writable, file-backed or anonymous, big enough to matter
        if (perms.size() >= 4 && perms[1] == 'w' && end - start >= 0x1000)
            out.push_back({start, end});
    }
    return out;
}

struct Angles {
    float pitch = 0.f, yaw = 0.f;
    bool ok = false;
};

Angles read_ref(const Memory& mem, std::uintptr_t base) {
    float m[16];
    if (!mem.read(base + kViewMatrixOff, m, sizeof m)) return {};
    float fx = m[12], fy = m[13], fz = m[14];
    const float len = std::sqrt(fx * fx + fy * fy + fz * fz);
    if (len < 1e-4f) return {};
    fx /= len;
    fy /= len;
    fz /= len;
    Angles a;
    a.yaw = std::atan2(fy, fx) * kRad2Deg;
    a.pitch = std::asin(std::clamp(fz, -1.f, 1.f)) * kRad2Deg;
    a.ok = true;
    return a;
}

float yaw_dist(float a, float b) {
    float d = std::fabs(a - b);
    d = std::fmod(d, 360.f);
    if (d > 180.f) d = 360.f - d;
    return d;
}

}  // namespace

int main(int argc, char** argv) {
    double seconds = argc > 1 ? std::atof(argv[1]) : 1.0;
    int max_tests = argc > 2 ? std::atoi(argv[2]) : 25;
    bool do_writes = false;
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "-w") do_writes = true;

    const auto pids = find_processes("cs2");
    if (pids.empty()) {
        std::printf("error: no cs2 process\n");
        return 1;
    }
    const int pid = pids.front();
    const auto base_opt = module_base(pid, "libclient.so");
    if (!base_opt) return 1;
    const std::uintptr_t base = *base_opt;
    Memory mem;
    mem.attach(pid);

    const Angles ref = read_ref(mem, base);
    if (!ref.ok) {
        std::printf("error: cannot read view matrix\n");
        return 1;
    }
    std::printf("pid=%d base=0x%lx ref pitch=%.3f yaw=%.3f\n", pid,
                static_cast<unsigned long>(base), ref.pitch, ref.yaw);

    // ---- scan all writable memory for strict current-angle triples ---------
    const auto ranges = writable_ranges(pid);
    std::printf("writable ranges: %zu\n", ranges.size());

    std::vector<std::uintptr_t> cands;
    std::vector<std::uint8_t> buf(1u << 20);
    std::uintptr_t total = 0;
    for (const auto& r : ranges) {
        total += r.end - r.start;
        std::uintptr_t a = r.start;
        while (a + 12 <= r.end && cands.size() < 3000) {
            std::size_t chunk = std::min<std::size_t>(buf.size(),
                                                      static_cast<std::size_t>(r.end - a));
            chunk &= ~3u;
            if (chunk < 12) break;
            if (!mem.read(a, buf.data(), chunk)) {
                a += 0x1000;
                continue;
            }
            for (std::size_t o = 0; o + 12 <= chunk; o += 4) {
                float p, y, rl;
                std::memcpy(&p, buf.data() + o, 4);
                std::memcpy(&y, buf.data() + o + 4, 4);
                std::memcpy(&rl, buf.data() + o + 8, 4);
                if (std::isnan(p) || std::isnan(y) || std::isnan(rl)) continue;
                // hard angle-like filter: kills huge-float false positives
                if (p < -89.5f || p > 89.5f) continue;
                if (y < -180.f || y > 180.f) continue;
                if (std::fabs(rl) > 0.75f) continue;
                const bool pitch_ok =
                    std::fabs(p - ref.pitch) < 0.75f ||
                    std::fabs(p + ref.pitch) < 0.75f;
                if (!pitch_ok) continue;
                if (yaw_dist(y, ref.yaw) > 0.75f) continue;
                cands.push_back(a + o);
            }
            a += chunk;
        }
    }
    std::sort(cands.begin(), cands.end());
    cands.erase(std::unique(cands.begin(), cands.end()), cands.end());
    std::printf("scanned %zu MB, strict matches: %zu\n", total >> 20, cands.size());
    for (const auto c : cands) {
        float v[3];
        if (mem.read(c, v, sizeof v))
            std::printf("  0x%lx (client+0x%lx): (%.3f %.3f %.3f)\n",
                        static_cast<unsigned long>(c),
                        static_cast<unsigned long>(c >= base ? c - base : 0),
                        v[0], v[1], v[2]);
    }

    // ---- continuous-write test each candidate (opt-in with -w) -------------
    if (!do_writes) {
        std::printf("\nscan only. rerun with -w to write-test candidates "
                    "(yanks the camera; keep the mouse still).\n");
        return 0;
    }
    std::printf("\n== write tests (keep mouse STILL; ~%.0fs each) ==\n",
                seconds + 0.5);
    const float target_yaw = ref.yaw + 45.f;
    int tested = 0;
    for (const auto c : cands) {
        if (tested >= max_tests) break;
        float v[3];
        if (!mem.read(c, v, sizeof v)) continue;
        const float saved[3] = {v[0], v[1], v[2]};
        const Angles start = read_ref(mem, base);
        if (!start.ok) continue;

        const auto t0 = std::chrono::steady_clock::now();
        while (std::chrono::duration<double>(
                   std::chrono::steady_clock::now() - t0).count() < seconds) {
            const float w[3] = {v[0], target_yaw, 0.f};
            mem.write(c, w, sizeof w);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const Angles end = read_ref(mem, base);
        mem.write(c, saved, sizeof saved);

        const float dy = yaw_dist(end.yaw, start.yaw);
        const bool steered = dy > 8.f;
        std::printf("  0x%lx (client+0x%lx): yaw %.1f -> %.1f (delta %+.1f) "
                    "%s\n",
                    static_cast<unsigned long>(c),
                    static_cast<unsigned long>(c >= base ? c - base : 0),
                    start.yaw, end.yaw, end.yaw - start.yaw,
                    steered ? "*** STEERED ***" : "");
        tested++;
        if (steered)
            std::printf("    >>> CAMERA STEERED: this slot drives the camera!\n");
    }
    return 0;
}
