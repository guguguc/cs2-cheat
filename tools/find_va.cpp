// find_va — locate the live view-angle QAngle global in the running native
// Linux CS2 process, without trusting any (possibly stale) dumper offset.
//
// Ground truth: the view-projection matrix at client+0x4809800 (the same
// matrix the ESP world-to-screen uses). In the game's row-major P*V layout
// the LAST ROW of VP equals the camera forward vector, so
//     yaw   = atan2(fwd.y, fwd.x)
//     pitch = asin(fwd.z)
// (Source convention: yaw 0 faces +X, positive yaw turns toward +Y, Z up.)
//
// Phase 1: one pass over every writable mapping of libclient.so collecting
//          all "angle-like" float triples (pitch, yaw, roll).
// Phase 2: for N seconds sample each candidate + the view matrix ~30 ms apart;
//          the real view-angle global is the candidate whose (pitch, yaw)
//          tracks the matrix-derived angles while the player looks around.
//
// Usage:  find_va [seconds] [tolerance_deg] [-w]
//   -w   after finding the best candidate, write +3 deg to it and check that
//        the view matrix actually rotates (definitive, opt-in).
//
// While it runs: be in a match, alive, and keep moving the mouse.

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

constexpr std::uintptr_t kViewMatrixOff = 0x4809800;  // RVA in libclient.so
constexpr std::uintptr_t kCSGOInputOff  = 0x4576E98;  // global ptr -> CCSGOInput
constexpr std::uintptr_t kStaleVAOff    = 0x45773E0;  // known-stale view angles
constexpr double kPi = 3.14159265358979323846;
constexpr float kRad2Deg = 180.f / static_cast<float>(kPi);

struct Range {
    std::uintptr_t start, end;
};

std::vector<Range> writable_ranges(int pid, const std::string& mod) {
    std::vector<Range> out;
    std::ifstream maps("/proc/" + std::to_string(pid) + "/maps");
    std::string line;
    while (std::getline(maps, line)) {
        const auto slash = line.rfind('/');
        if (slash == std::string::npos) continue;
        std::string path = line.substr(slash + 1);
        const auto sp = path.find(' ');
        if (sp != std::string::npos) path = path.substr(0, sp);
        if (path != mod) continue;

        const auto dash = line.find('-');
        const auto spc = line.find(' ');
        if (dash == std::string::npos || spc == std::string::npos) continue;
        const std::uintptr_t start =
            std::stoull(line.substr(0, dash), nullptr, 16);
        const std::uintptr_t end =
            std::stoull(line.substr(dash + 1, spc - dash - 1), nullptr, 16);
        const std::string perms = line.substr(spc + 1, 4);
        if (perms.size() >= 4 && perms[1] == 'w' && end > start)
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
    float fx = m[12], fy = m[13], fz = m[14];  // VP last row == forward
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

bool angle_like(float p, float y, float r) {
    if (std::isnan(p) || std::isnan(y) || std::isnan(r)) return false;
    if (std::isinf(p) || std::isinf(y) || std::isinf(r)) return false;
    if (p < -89.5f || p > 89.5f) return false;   // pitch must be a pitch
    if (r < -1.5f || r > 1.5f) return false;     // roll ~ 0
    if (y < -360.f || y > 360.f) return false;   // yaw sane range
    return true;
}

// Shortest angular distance between two angles, wrapping yaw at 180.
float ang_dist(float a, float b, bool wrap) {
    float d = std::fabs(a - b);
    if (wrap) {
        d = std::fmod(d, 360.f);
        if (d > 180.f) d = 360.f - d;
    }
    return d;
}

float qangle_dist(float p, float y, const Angles& ref) {
    // try (pitch, yaw) and (yaw, pitch) orders
    float d1 = ang_dist(p, ref.pitch, false) + ang_dist(y, ref.yaw, true);
    float d2 = ang_dist(y, ref.pitch, false) + ang_dist(p, ref.yaw, true);
    return std::min(d1, d2);
}

struct CandStat {
    std::uintptr_t addr = 0;
    float p0 = 0.f, y0 = 0.f, r0 = 0.f;
    bool inited = false;
    bool changed = false;
    float best = 1e9f;
    int near = 0;
    int nsamp = 0;
};

}  // namespace

int main(int argc, char** argv) {
    const double seconds = argc > 1 ? std::atof(argv[1]) : 6.0;
    const double tol = argc > 2 ? std::atof(argv[2]) : 1.5;
    const bool write_test =
        (argc > 3 && std::string(argv[3]) == "-w") ||
        (argc > 4 && std::string(argv[4]) == "-w");

    const auto pids = find_processes("cs2");
    if (pids.empty()) {
        std::printf("error: no cs2 process found\n");
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

    const Angles ref0 = read_ref(mem, base);
    if (!ref0.ok) {
        std::printf("error: cannot read view matrix @ 0x%lx\n",
                    static_cast<unsigned long>(base + kViewMatrixOff));
        return 1;
    }
    std::printf("view matrix @ 0x%lx  ref pitch=%.3f yaw=%.3f\n",
                static_cast<unsigned long>(base + kViewMatrixOff),
                ref0.pitch, ref0.yaw);

    float stale[3] = {0, 0, 0};
    if (mem.read(base + kStaleVAOff, stale, sizeof stale))
        std::printf("stale dwViewAngles(0x%lx) = (%.3f %.3f %.3f)\n",
                    static_cast<unsigned long>(base + kStaleVAOff),
                    stale[0], stale[1], stale[2]);

    // CCSGOInput pointer (global holds a pointer to the input object).
    std::uintptr_t input_obj = 0;
    mem.read(base + kCSGOInputOff, &input_obj, sizeof input_obj);
    std::printf("dwCSGOInput(0x%lx) -> 0x%lx\n",
                static_cast<unsigned long>(base + kCSGOInputOff),
                static_cast<unsigned long>(input_obj));

    // ---- phase 1: collect angle-like triples -------------------------------
    const auto ranges = writable_ranges(pid, "libclient.so");
    std::printf("writable ranges: %zu\n", ranges.size());

    std::vector<std::uintptr_t> cands;
    cands.reserve(100000);
    std::vector<std::uint8_t> buf(1u << 20);
    for (const auto& r : ranges) {
        std::uintptr_t a = r.start;
        while (a + 12 <= r.end) {
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
                if (angle_like(p, y, rl)) cands.push_back(a + o);
            }
            a += chunk;
        }
    }
    std::sort(cands.begin(), cands.end());
    std::printf("candidates: %zu\n", cands.size());
    if (cands.empty()) {
        std::printf("no angle-like triples found; aborting\n");
        return 1;
    }

    // ---- group candidates by 4KB page --------------------------------------
    struct Page {
        std::uintptr_t page;
        std::vector<std::size_t> idxs;
    };
    std::vector<Page> pages;
    for (std::size_t i = 0; i < cands.size(); ++i) {
        const std::uintptr_t pg = cands[i] & ~0xFFFul;
        if (pages.empty() || pages.back().page != pg)
            pages.push_back({pg, {}});
        pages.back().idxs.push_back(i);
    }
    std::printf("pages: %zu\n", pages.size());

    // ---- phase 2: sample everything for `seconds` --------------------------
    std::vector<CandStat> st(cands.size());
    for (std::size_t i = 0; i < cands.size(); ++i) st[i].addr = cands[i];
    std::vector<std::array<float, 3>> vals(cands.size());
    std::vector<bool> have(cands.size(), false);

    std::vector<std::uint8_t> pg(4096);
    const auto t0 = std::chrono::steady_clock::now();
    float max_view_move = 0.f;
    Angles prev_ref = ref0;
    int iters = 0;

    while (true) {
        const auto now = std::chrono::steady_clock::now();
        const double el = std::chrono::duration<double>(now - t0).count();
        if (el >= seconds) break;

        // pass 1: read every touched page once, extract candidate triples
        for (const auto& pg_ : pages) {
            const std::uintptr_t s = pg_.page;
            if (mem.read(s, pg.data(), 4096)) {
                for (const std::size_t idx : pg_.idxs) {
                    const std::size_t o = cands[idx] - s;
                    if (o + 12 > 4096) continue;
                    float p, y, rl;
                    std::memcpy(&p, pg.data() + o, 4);
                    std::memcpy(&y, pg.data() + o + 4, 4);
                    std::memcpy(&rl, pg.data() + o + 8, 4);
                    vals[idx] = {p, y, rl};
                    have[idx] = true;
                    CandStat& c = st[idx];
                    if (!c.inited) {
                        c.p0 = p;
                        c.y0 = y;
                        c.r0 = rl;
                        c.inited = true;
                    } else if (std::fabs(p - c.p0) > 0.2f ||
                               std::fabs(y - c.y0) > 0.2f ||
                               std::fabs(rl - c.r0) > 0.2f) {
                        c.changed = true;
                    }
                    c.nsamp++;
                }
            } else {
                // page read failed (straddles a mapping edge): read individually
                for (const std::size_t idx : pg_.idxs) {
                    float v[3];
                    if (!mem.read(cands[idx], v, sizeof v)) continue;
                    vals[idx] = {v[0], v[1], v[2]};
                    have[idx] = true;
                    CandStat& c = st[idx];
                    if (!c.inited) {
                        c.p0 = v[0];
                        c.y0 = v[1];
                        c.r0 = v[2];
                        c.inited = true;
                    } else if (std::fabs(v[0] - c.p0) > 0.2f ||
                               std::fabs(v[1] - c.y0) > 0.2f ||
                               std::fabs(v[2] - c.r0) > 0.2f) {
                        c.changed = true;
                    }
                    c.nsamp++;
                }
            }
        }

        // pass 2: ground-truth matrix, then score this iteration's values
        const Angles ref = read_ref(mem, base);
        if (ref.ok) {
            max_view_move = std::max(max_view_move,
                                     ang_dist(ref.yaw, prev_ref.yaw, true));
            max_view_move = std::max(max_view_move,
                                     ang_dist(ref.pitch, prev_ref.pitch, false));
            prev_ref = ref;
            for (std::size_t i = 0; i < cands.size(); ++i) {
                if (!have[i]) continue;
                CandStat& c = st[i];
                const float d = qangle_dist(vals[i][0], vals[i][1], ref);
                if (d < c.best) c.best = d;
                if (d < tol) c.near++;
            }
            if (iters % 20 == 0)
                std::printf("  t=%.1fs ref=(pitch %.2f yaw %.2f)\n", el,
                            ref.pitch, ref.yaw);
        }
        iters++;
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }

    std::printf("\nsamples=%d view moved (max delta %.2f deg)\n", iters,
                max_view_move);
    if (max_view_move < 0.5)
        std::printf("WARNING: view barely moved - move the mouse while probing!\n");

    // ---- report -------------------------------------------------------------
    struct Hit {
        CandStat* c;
        float d;
        int near;
    };
    std::vector<Hit> hits;
    for (auto& c : st) {
        if (!c.inited) continue;
        if (c.best < 3.0) hits.push_back({&c, c.best, c.near});
    }
    std::sort(hits.begin(), hits.end(),
              [](const Hit& a, const Hit& b) { return a.d < b.d; });

    std::printf("\n== candidates tracking the view matrix ==\n");
    int shown = 0;
    for (const auto& h : hits) {
        if (shown >= 25) break;
        const std::uintptr_t rva = h.c->addr - base;
        std::printf("  0x%lx (rva 0x%lx)  init=(%.2f %.2f %.2f)  "
                    "best_d=%.3f near=%d changed=%d%s\n",
                    static_cast<unsigned long>(h.c->addr),
                    static_cast<unsigned long>(rva),
                    h.c->p0, h.c->y0, h.c->r0, h.d, h.near,
                    h.c->changed ? 1 : 0,
                    (rva >= kStaleVAOff - 0x100000 && rva <= kStaleVAOff + 0x100000)
                        ? "  <-- near stale offset" : "");
        shown++;
    }

    // angle-like pairs inside the CCSGOInput object window (CUserCmd path)
    if (input_obj) {
        std::printf("\n== CCSGOInput object window (offset, p,y,r) ==\n");
        std::vector<std::uint8_t> ibuf(0x1000);
        if (mem.read(input_obj - 0x40, ibuf.data(), ibuf.size())) {
            for (std::size_t o = 0; o + 12 <= ibuf.size(); o += 4) {
                float p, y, rl;
                std::memcpy(&p, ibuf.data() + o, 4);
                std::memcpy(&y, ibuf.data() + o + 4, 4);
                std::memcpy(&rl, ibuf.data() + o + 8, 4);
                if (angle_like(p, y, rl))
                    std::printf("  input+0x%03zx: (%.2f %.2f %.2f)\n",
                                o - 0x40, p, y, rl);
            }
        }
    }

    // ---- optional write test -------------------------------------------------
    if (write_test && !hits.empty()) {
        const std::uintptr_t addr = hits.front().c->addr;
        std::printf("\n== write test @ 0x%lx (rva 0x%lx) ==\n",
                    static_cast<unsigned long>(addr),
                    static_cast<unsigned long>(addr - base));
        float v[3];
        if (mem.read(addr, v, sizeof v)) {
            const Angles before = read_ref(mem, base);
            const float saved[3] = {v[0], v[1], v[2]};
            v[0] += 3.f;
            v[1] += 3.f;
            mem.write(addr, v, sizeof v);
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            const Angles after = read_ref(mem, base);
            mem.write(addr, saved, sizeof saved);
            std::printf("  before=(%.2f %.2f) after=(%.2f %.2f)  "
                        "pitch delta %.2f yaw delta %.2f\n",
                        before.pitch, before.yaw, after.pitch, after.yaw,
                        after.pitch - before.pitch, after.yaw - before.yaw);
            if (std::fabs(after.pitch - before.pitch) > 1.5f ||
                std::fabs(after.yaw - before.yaw) > 1.5f)
                std::printf("  RESULT: camera rotated -> this is the writable "
                            "view-angle global\n");
            else
                std::printf("  RESULT: camera did NOT rotate (not the camera "
                            "source; try the input-cmd path)\n");
        }
    }

    return 0;
}
