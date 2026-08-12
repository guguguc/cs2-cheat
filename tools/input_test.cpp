// input_test - find which memory slot the CS2 camera actually reads its
// view angles from, and which slot is the writable aimbot target.
//
// Knowns from the earlier scan (find_va):
//   * client + 0x4576E98  (dwCSGOInput)  holds a pointer to the input object
//   * the input object holds the CURRENT view angles at +0x9C, +0x548, +0x5F0
//     (all read (0, -160, 0) while the view matrix said yaw = -160)
//   * client + 0x45773E0  (a2x dwViewAngles = dwCSGOInput + 0x548) does NOT
//     track the camera
//
// Phase A: sample the matrix + a window around the input object + the stale
//          global for a few seconds; report every float triple that tracks
//          the camera (i.e. its pitch/yaw match the matrix-derived angles).
// Phase B (-w): for each tracking triple (and the stale global), write +3 deg
//          and observe whether the camera actually rotates. KEEP THE MOUSE
//          STILL during the write test or the result is meaningless.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "memory.h"
#include "process.h"

namespace {

constexpr std::uintptr_t kViewMatrixOff = 0x4809800;
constexpr std::uintptr_t kCSGOInputOff  = 0x4576E98;
constexpr std::uintptr_t kStaleVAOff    = 0x45773E0;
constexpr double kPi = 3.14159265358979323846;
constexpr float kRad2Deg = 180.f / static_cast<float>(kPi);
constexpr std::uintptr_t kInputWindow = 0x1000;  // input_obj - 0x100 .. +0x1000

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

float ang_dist(float a, float b, bool wrap) {
    float d = std::fabs(a - b);
    if (wrap) {
        d = std::fmod(d, 360.f);
        if (d > 180.f) d = 360.f - d;
    }
    return d;
}

struct Track {
    std::uintptr_t addr = 0;
    float p0 = 0.f, y0 = 0.f, r0 = 0.f;
    bool inited = false;
    float best = 1e9f;
    int near = 0;      // samples within 1.5 deg of the matrix
    int nsamp = 0;
    int changed = 0;   // samples where the triple moved > 0.3 deg
};

}  // namespace

int main(int argc, char** argv) {
    double seconds = argc > 1 ? std::atof(argv[1]) : 8.0;
    bool write_test = false;
    std::uintptr_t single_rva = 0;
    bool dump = false;
    std::uintptr_t cont_rva = 0;
    std::uintptr_t raw_addr = 0;
    std::size_t raw_len = 0;
    float cont_yaw = 90.f;
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "-w") write_test = true;
        else if (std::string(argv[i]) == "-t" && i + 1 < argc)
            single_rva = std::strtoull(argv[++i], nullptr, 0);
        else if (std::string(argv[i]) == "-c" && i + 2 < argc) {
            cont_rva = std::strtoull(argv[++i], nullptr, 0);
            cont_yaw = static_cast<float>(std::atof(argv[++i]));
        }
        else if (std::string(argv[i]) == "-r" && i + 2 < argc) {
            raw_addr = std::strtoull(argv[++i], nullptr, 0);
            raw_len = std::strtoull(argv[++i], nullptr, 0);
        }
        else if (std::string(argv[i]) == "-d") dump = true;

    const auto pids = find_processes("cs2");
    if (pids.empty()) {
        std::printf("error: no cs2 process\n");
        return 1;
    }
    const int pid = pids.front();
    const auto base_opt = module_base(pid, "libclient.so");
    if (!base_opt) {
        std::printf("error: libclient.so not mapped\n");
        return 1;
    }
    const std::uintptr_t base = *base_opt;
    Memory mem;
    mem.attach(pid);

    std::printf("pid=%d libclient_base=0x%lx\n", pid,
                static_cast<unsigned long>(base));

    std::uintptr_t input_obj = 0;
    mem.read(base + kCSGOInputOff, &input_obj, sizeof input_obj);
    std::printf("dwCSGOInput(0x%lx) -> input_obj=0x%lx (client+0x%lx)\n",
                static_cast<unsigned long>(base + kCSGOInputOff),
                static_cast<unsigned long>(input_obj),
                static_cast<unsigned long>(input_obj - base));

    const Angles ref0 = read_ref(mem, base);
    std::printf("initial ref: pitch=%.3f yaw=%.3f\n", ref0.pitch, ref0.yaw);

    // ---- raw dump mode ------------------------------------------------------
    // input_test -r ADDR LEN : print ADDR..ADDR+LEN as float|int32|int64.
    if (raw_addr) {
        const std::uintptr_t addr = raw_addr;
        std::vector<std::uint8_t> b(raw_len);
        if (!mem.read(addr, b.data(), b.size())) {
            std::printf("cannot read 0x%lx len %zu\n",
                        static_cast<unsigned long>(addr), raw_len);
            return 1;
        }
        for (std::size_t o = 0; o + 4 <= b.size(); o += 4) {
            float f;
            std::int32_t i32;
            std::memcpy(&f, b.data() + o, 4);
            std::memcpy(&i32, b.data() + o, 4);
            std::printf("  +0x%03zx: 0x%08x  int=%9d  float=%12.4f\n",
                        o, i32, i32, static_cast<double>(f));
        }
        return 0;
    }

    // ---- continuous write mode ---------------------------------------------
    // input_test -c RVA [yaw_target]: hammer a slot with a fixed QAngle every
    // 10 ms for ~2 s and report where the camera ends up. A single write is
    // often overwritten by the game; continuous writes reveal the real slot.
    if (cont_rva) {
        const std::uintptr_t addr = base + cont_rva;
        const float target_yaw = cont_yaw;
        std::printf("\ncontinuous write: 0x%lx (client+0x%lx), target yaw=%+.0f "
                    "for ~2s. Keep the mouse STILL!\n",
                    static_cast<unsigned long>(addr),
                    static_cast<unsigned long>(cont_rva), target_yaw);

        float v[3] = {0.f, 0.f, 0.f};
        mem.read(addr, v, sizeof v);
        const float saved[3] = {v[0], v[1], v[2]};
        const Angles start = read_ref(mem, base);

        const auto t0 = std::chrono::steady_clock::now();
        while (std::chrono::duration<double>(
                   std::chrono::steady_clock::now() - t0).count() < 2.0) {
            const float w[3] = {v[0], target_yaw, 0.f};
            mem.write(addr, w, sizeof w);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const Angles end = read_ref(mem, base);
        mem.write(addr, saved, sizeof saved);

        std::printf("camera yaw: %.2f -> %.2f (delta %+.2f), pitch %.2f -> "
                    "%.2f\n",
                    start.yaw, end.yaw, end.yaw - start.yaw,
                    start.pitch, end.pitch);
        const bool steered =
            std::fabs(ang_dist(end.yaw, target_yaw, true)) < 10.f ||
            std::fabs(end.yaw - start.yaw) > 10.f;
        std::printf("%s\n", steered
                                ? "*** CAMERA STEERED -> THIS IS THE WRITE "
                                  "TARGET ***"
                                : "no effect");
        return 0;
    }

    // ---- single-slot linearity probe ---------------------------------------
    // input_test -t 0x481aed0 : write +3 / +30 / +90 to yaw of that slot and
    // verify the camera follows by the same amount (absolute QAngle) or not.
    if (single_rva) {
        const std::uintptr_t addr = base + single_rva;
        float v[3];
        if (!mem.read(addr, v, sizeof v)) {
            std::printf("cannot read 0x%lx\n",
                        static_cast<unsigned long>(addr));
            return 1;
        }
        std::printf("\nsingle-slot linearity probe: 0x%lx (client+0x%lx) "
                    "current=(%.3f %.3f %.3f)\n",
                    static_cast<unsigned long>(addr),
                    static_cast<unsigned long>(single_rva), v[0], v[1], v[2]);
        std::printf("keep the mouse STILL the whole time!\n");

        for (const float dy : {3.f, 30.f, 90.f, -45.f}) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            const Angles base_ref = read_ref(mem, base);
            if (!base_ref.ok) continue;

            const float saved[3] = {v[0], v[1], v[2]};
            float w[3] = {v[0], v[1] + dy, 0.f};
            if (w[1] > 180.f) w[1] -= 360.f;
            if (w[1] < -180.f) w[1] += 360.f;
            mem.write(addr, w, sizeof w);

            float max_dy = 0.f, max_dp = 0.f;
            for (int k = 0; k < 8; ++k) {
                std::this_thread::sleep_for(std::chrono::milliseconds(40));
                const Angles a = read_ref(mem, base);
                if (!a.ok) continue;
                max_dp = std::max(max_dp, ang_dist(a.pitch, base_ref.pitch, false));
                max_dy = std::max(max_dy, ang_dist(a.yaw, base_ref.yaw, true));
            }
            mem.write(addr, saved, sizeof saved);

            // observe settling after restore
            float settle = 0.f;
            for (int k = 0; k < 6; ++k) {
                std::this_thread::sleep_for(std::chrono::milliseconds(40));
                const Angles a = read_ref(mem, base);
                if (!a.ok) continue;
                settle = std::max(settle, ang_dist(a.yaw, base_ref.yaw, true));
            }

            std::printf("  write yaw %+6.1f -> camera yaw moved max %6.1f  "
                        "pitch %5.1f  (settled back to %.1f)\n",
                        dy, max_dy, max_dp, settle);
        }
        return 0;
    }

    float stale0[3] = {0, 0, 0};
    if (mem.read(base + kStaleVAOff, stale0, sizeof stale0))
        std::printf("stale dwViewAngles(0x%lx) = (%.3f %.3f %.3f)\n",
                    static_cast<unsigned long>(base + kStaleVAOff),
                    stale0[0], stale0[1], stale0[2]);

    // ---- dump mode: find the CUserCmd array --------------------------------
    if (dump) {
        const Angles ref = read_ref(mem, base);
        std::printf("ref pitch=%.3f yaw=%.3f (matrix; stored pitch has the "
                    "opposite sign)\n", ref.pitch, ref.yaw);

        std::printf("\n== input object pointer fields (uint64) ==\n");
        std::vector<std::uint64_t> ptrs(64, 0);
        for (int i = 0; i < 64; ++i) {
            mem.read(input_obj + static_cast<std::uintptr_t>(i) * 8,
                     &ptrs[i], 8);
            std::printf("  input+0x%03x: 0x%016lx\n", i * 8,
                        static_cast<unsigned long>(ptrs[i]));
        }

        // For each plausible pointer, scan its target for a QAngle that
        // matches the current camera angle (pitch sign-agnostic, yaw wrapped).
        auto near_pitch = [](float a, float b) {
            return std::fabs(a - b) < 1.0f || std::fabs(a + b) < 1.0f;
        };
        auto near_yaw = [](float a, float b) {
            float d = std::fabs(a - b);
            d = std::fmod(d, 360.f);
            if (d > 180.f) d = 360.f - d;
            return d < 1.0f;
        };

        std::printf("\n== scanning pointer targets for current angles ==\n");
        std::vector<std::uint8_t> tbuf(0x2000);
        for (int i = 0; i < 64; ++i) {
            const std::uint64_t p = ptrs[i];
            if (p < 0x1000 || p > 0x7fffffffffffUL) continue;
            if (!mem.read(p, tbuf.data(), tbuf.size())) continue;
            for (std::size_t o = 0; o + 12 <= tbuf.size(); o += 4) {
                float a, b, c;
                std::memcpy(&a, tbuf.data() + o, 4);
                std::memcpy(&b, tbuf.data() + o + 4, 4);
                std::memcpy(&c, tbuf.data() + o + 8, 4);
                if (std::isnan(a) || std::isnan(b) || std::isnan(c)) continue;
                if (std::fabs(c) > 1.5f) continue;
                if ((near_pitch(a, ref.pitch) && near_yaw(b, ref.yaw)) ||
                    (near_pitch(b, ref.pitch) && near_yaw(a, ref.yaw))) {
                    std::printf("  input+0x%03x ptr 0x%lx -> target+0x%03zx: "
                                "(%.3f %.3f %.3f)\n",
                                i * 8, static_cast<unsigned long>(p), o,
                                a, b, c);
                }
            }
        }

        // raw view of the region holding the angle copies
        std::printf("\n== input+0x50 .. input+0x110 (int32 | float) ==\n");
        for (std::uintptr_t o = 0x50; o < 0x110; o += 4) {
            std::uint32_t u = 0;
            mem.read(input_obj + o, &u, 4);
            float f;
            std::memcpy(&f, &u, 4);
            std::printf("  +0x%03lx: 0x%08x  int=%d  float=%.3f\n",
                        static_cast<unsigned long>(o), u, static_cast<int>(u), f);
        }
        return 0;
    }

    // ---- candidate addresses ------------------------------------------------
    // window = [input_obj-0x100, input_obj+kInputWindow), 4-aligned
    std::vector<std::uintptr_t> cands;
    const std::uintptr_t win_start = input_obj - 0x100;
    for (std::uintptr_t a = win_start; a + 12 <= input_obj + kInputWindow; a += 4)
        cands.push_back(a);
    cands.push_back(base + kStaleVAOff);  // the a2x global, always test it
    std::printf("candidates: %zu\n", cands.size());

    std::vector<Track> st(cands.size());
    for (std::size_t i = 0; i < cands.size(); ++i) st[i].addr = cands[i];
    std::vector<std::array<float, 3>> vals(cands.size());

    // ---- phase A: sample ----------------------------------------------------
    const auto t0 = std::chrono::steady_clock::now();
    Angles prev = ref0;
    float max_view_move = 0.f;
    int iters = 0;

    while (true) {
        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - t0).count() >= seconds) break;

        for (std::size_t i = 0; i < cands.size(); ++i) {
            float v[3];
            if (!mem.read(cands[i], v, sizeof v)) continue;
            vals[i] = {v[0], v[1], v[2]};
            Track& c = st[i];
            if (!c.inited) {
                c.p0 = v[0];
                c.y0 = v[1];
                c.r0 = v[2];
                c.inited = true;
            }
            c.nsamp++;
        }

        const Angles ref = read_ref(mem, base);
        if (ref.ok) {
            max_view_move = std::max(max_view_move,
                                     ang_dist(ref.yaw, prev.yaw, true));
            max_view_move = std::max(max_view_move,
                                     ang_dist(ref.pitch, prev.pitch, false));
            prev = ref;
            for (std::size_t i = 0; i < cands.size(); ++i) {
                if (!st[i].inited) continue;
                const float d1 = ang_dist(vals[i][0], ref.pitch, false) +
                                 ang_dist(vals[i][1], ref.yaw, true);
                const float d2 = ang_dist(vals[i][1], ref.pitch, false) +
                                 ang_dist(vals[i][0], ref.yaw, true);
                const float d = std::min(d1, d2);
                if (d < st[i].best) st[i].best = d;
                if (d < 1.5f) st[i].near++;
                if (ang_dist(vals[i][0], st[i].p0, false) > 0.3f ||
                    ang_dist(vals[i][1], st[i].y0, true) > 0.3f)
                    st[i].changed++;
            }
        }
        iters++;
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }

    std::printf("\nsamples=%d view moved %.2f deg (move the mouse while "
                "sampling!)\n", iters, max_view_move);

    std::vector<std::size_t> hits;
    std::printf("\n== triples tracking the camera ==\n");
    for (std::size_t i = 0; i < cands.size(); ++i) {
        const Track& c = st[i];
        if (!c.inited || c.best > 1.5f) continue;
        hits.push_back(i);
        const std::uintptr_t rva = c.addr - base;
        std::printf("  0x%lx (client+0x%lx) init=(%.2f %.2f %.2f) best=%.3f "
                    "near=%d/%d changed=%d\n",
                    static_cast<unsigned long>(c.addr),
                    static_cast<unsigned long>(rva),
                    c.p0, c.y0, c.r0, c.best, c.near, c.nsamp, c.changed);
    }
    if (hits.empty())
        std::printf("  (none - did you move the mouse during sampling?)\n");

    // ---- phase B: write test ------------------------------------------------
    if (!write_test) {
        std::printf("\nrerun with -w to write-test the tracking slots "
                    "(keep the mouse STILL during the write test)\n");
        return 0;
    }

    std::printf("\n== write test (keep mouse still!) ==\n");
    for (const std::size_t i : hits) {
        const std::uintptr_t addr = cands[i];
        const Track& c = st[i];
        // Only write-test slots that (a) look like a real QAngle and (b) were
        // seen tracking the mouse. Writing +3 deg into a random slot that
        // actually holds a pointer would corrupt input state and crash.
        const bool angle_like = std::fabs(c.p0) <= 89.5f &&
                                std::fabs(c.y0) <= 180.f &&
                                std::fabs(c.r0) <= 3.f;
        const bool tracked = c.near > 20;  // demonstrably tracks the camera
        if (!angle_like || !tracked) {
            std::printf("  0x%lx (client+0x%lx): skipped (not a clean "
                        "tracked QAngle)\n",
                        static_cast<unsigned long>(addr),
                        static_cast<unsigned long>(addr - base));
            continue;
        }
        float v[3];
        if (!mem.read(addr, v, sizeof v)) continue;

        const Angles base_ref = read_ref(mem, base);
        const float saved[3] = {v[0], v[1], v[2]};

        // write +3 deg (clamped), then watch the camera for ~350 ms
        float w[3] = {v[0] + 3.f, v[1] + 3.f, 0.f};
        w[0] = std::clamp(w[0], -89.f, 89.f);
        if (w[1] > 180.f) w[1] -= 360.f;
        if (w[1] < -180.f) w[1] += 360.f;

        mem.write(addr, w, sizeof w);

        float max_dp = 0.f, max_dy = 0.f;
        for (int k = 0; k < 18; ++k) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            const Angles a = read_ref(mem, base);
            if (!a.ok) continue;
            max_dp = std::max(max_dp, ang_dist(a.pitch, base_ref.pitch, false));
            max_dy = std::max(max_dy, ang_dist(a.yaw, base_ref.yaw, true));
        }
        mem.write(addr, saved, sizeof saved);

        const bool moved = max_dp > 1.5f || max_dy > 1.5f;
        std::printf("  0x%lx (client+0x%lx): wrote +3 -> camera moved "
                    "pitch %.2f yaw %.2f  %s\n",
                    static_cast<unsigned long>(addr),
                    static_cast<unsigned long>(addr - base),
                    max_dp, max_dy, moved ? "*** WRITABLE ***" : "no");
    }

    // Always write-test the a2x global too (client + dwViewAngles).
    {
        const std::uintptr_t addr = base + kStaleVAOff;
        float v[3];
        if (mem.read(addr, v, sizeof v)) {
            const Angles base_ref = read_ref(mem, base);
            const float saved[3] = {v[0], v[1], v[2]};
            float w[3] = {v[0] + 3.f, v[1] + 3.f, 0.f};
            w[0] = std::clamp(w[0], -89.f, 89.f);
            if (w[1] > 180.f) w[1] -= 360.f;
            if (w[1] < -180.f) w[1] += 360.f;
            mem.write(addr, w, sizeof w);
            float max_dp = 0.f, max_dy = 0.f;
            for (int k = 0; k < 18; ++k) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                const Angles a = read_ref(mem, base);
                if (!a.ok) continue;
                max_dp = std::max(max_dp, ang_dist(a.pitch, base_ref.pitch, false));
                max_dy = std::max(max_dy, ang_dist(a.yaw, base_ref.yaw, true));
            }
            mem.write(addr, saved, sizeof saved);
            const bool moved = max_dp > 1.5f || max_dy > 1.5f;
            std::printf("  0x%lx (client+0x%lx, a2x dwViewAngles): wrote +3 "
                        "-> camera moved pitch %.2f yaw %.2f  %s\n",
                        static_cast<unsigned long>(addr),
                        static_cast<unsigned long>(addr - base),
                        max_dp, max_dy, moved ? "*** WRITABLE ***" : "no");
        }
    }
    return 0;
}
