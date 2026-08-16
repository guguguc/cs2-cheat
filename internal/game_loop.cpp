#include "game_loop.h"

#include "cfg.h"
#include "logger.h"
#include "mouse_device.h"
#include "overlay_ctx.h"
#include "process.h"

#include <mutex>
#include <thread>
#include <unistd.h>

namespace {

// deadlocked current_map(): the map name string pointer lives at global_vars
// + 0x198; reload the BVH whenever it changes.
std::string read_cstr(const Memory& mem, std::uintptr_t addr) {
    if (!addr) return {};
    char buf[128] = {0};
    mem.read(addr, buf, sizeof(buf) - 1);
    return std::string(buf);
}

}  // namespace

GameLoop::GameLoop(std::atomic<bool>& run)
    : run_(run), game_(mem_), aimbot_(game_), rcs_(game_), trigger_(game_) {}

bool GameLoop::init() {
    Logger::instance().log("data_thread: start\n");
    mem_.attach(getpid());

    if (!offsets_.attach(getpid())) {
        Logger::instance().log("entry: pattern resolve failed; retrying in background\n");
        for (int i = 0; i < 20 && !offsets_.ok(); ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            offsets_.attach(getpid());
        }
    }
    if (!offsets_.ok()) return false;
    const patterns::Resolved& off = offsets_.resolved();

    if (!game_.attach(off)) {
        Logger::instance().log("entry: game attach failed\n");
        return false;
    }
    Logger::instance().log("entry: attached, offsets ok, client base 0x%llx\n",
                           static_cast<unsigned long long>(game_.client_base()));

    // Input object: the live view angles are mirrored at +0x9C.
    const std::uintptr_t input_obj =
        mem_.read<std::uintptr_t>(game_.client_base() + Config::instance().offsets.dwCSGOInput)
            .value_or(0);
    if (input_obj) {
        game_.set_view_angle_source(input_obj + Config::instance().offsets.viewAngleOffset);  // real view angles
        g_mouse.set_input_obj(input_obj);
        g_mouse.set_sensitivity_convar(find_sensitivity_convar());
        // Create the virtual device lazily on first aim (in-match, so it never
        // collides with the game's raw-input init) and keep it alive (no
        // destroy -> no pointer-warp self-move on release).
        game_.set_angles_override([](const Vector3& a, bool) {
            g_mouse.move_to(a.x, a.y);
        });
        Logger::instance().log("aim: uinput ready (input_obj=0x%llx)\n",
                               static_cast<unsigned long long>(input_obj));
    } else {
        Logger::instance().log("input: no input object (dwCSGOInput null)\n");
        game_.set_angles_override([](const Vector3&, bool) {});
    }
    return true;
}

void GameLoop::check_bvh(const patterns::Resolved& off) {
    // 200 ms throttle; reload only after entering a match. Failed loads do not
    // remember the map name, so the next cycle retries automatically.
    if (std::chrono::steady_clock::now() - last_bvh_check_ < std::chrono::milliseconds(200))
        return;
    last_bvh_check_ = std::chrono::steady_clock::now();
    if (!off.globalVars || !off.vphysWorld) return;
    const auto gv = mem_.read<std::uintptr_t>(off.globalVars).value_or(0);
    if (!gv) return;
    const auto name_ptr = mem_.read<std::uintptr_t>(gv + 0x198).value_or(0);
    const std::string map = read_cstr(mem_, name_ptr);
    if (map == last_map_) return;
    last_map_ = map;
    if (!aimbot_.init_bvh(off.vphysWorld, /*force=*/true))
        last_map_.clear();  // failed: retry next 200 ms cycle
}

std::uintptr_t GameLoop::find_sensitivity_convar() const {
    // Scans the ICvar convar list for "sensitivity" (deadlocked-style).
    // libtier0.so exports VEngineCvar007 (interface table at base + kCvarIfaceOff);
    // each convar object has a name pointer at +0x0 and its float value at +0x58.
    const auto tier0 = module_base(mem_.pid(), "libtier0.so");
    if (!tier0) { Logger::instance().log("convar: no libtier0\n"); return 0; }
    constexpr std::uintptr_t kCvarIfaceOff = 1423200;  // VEngineCvar007 (dumper 2026-08-13)
    const auto iface = mem_.read<std::uintptr_t>(*tier0 + kCvarIfaceOff).value_or(0);
    if (!iface) { Logger::instance().log("convar: no cvar interface\n"); return 0; }
    const auto objects = mem_.read<std::uintptr_t>(iface + 0x50).value_or(0);
    const std::uint32_t count = mem_.read<std::uint32_t>(iface + 160).value_or(0);
    if (!objects || !count) { Logger::instance().log("convar: empty list (count=%u)\n", count); return 0; }
    for (std::uint32_t i = 0; i < count && i < 4096; ++i) {
        const auto obj = mem_.read<std::uintptr_t>(objects + i * 16).value_or(0);
        if (!obj) break;
        const auto name_addr = mem_.read<std::uintptr_t>(obj).value_or(0);
        if (!name_addr) continue;
        char buf[32] = {0};
        mem_.read(name_addr, buf, sizeof(buf) - 1);
        if (std::string(buf) == "sensitivity") {
            Logger::instance().log("convar: sensitivity at 0x%llx\n", static_cast<unsigned long long>(obj));
            return obj;
        }
    }
    Logger::instance().log("convar: sensitivity not found (count=%u)\n", count);
    return 0;
}

void GameLoop::run() {
    const patterns::Resolved& off = offsets_.resolved();
    while (run_.load()) {
        game_.update();
        check_bvh(off);  // deadlocked-style: 200 ms, map-change triggered

        // BVH visibility involves several remote memory reads and ray casts per
        // player. Keep it off the render/input lock and refresh it at a lower
        // rate so ESP coloring cannot stall the game's mouse or ImGui frame.
        Snapshot snapshot = game_.snapshot();
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_visibility_update_) {
            visibility_cache_.clear();
            for (const Player& p : snapshot.players)
                visibility_cache_[p.address] = aimbot_.visible(p);
            next_visibility_update_ = now + std::chrono::milliseconds(200);
        }
        for (Player& p : snapshot.players) {
            const auto it = visibility_cache_.find(p.address);
            p.visible = it != visibility_cache_.end() && it->second;
        }

        {
            std::lock_guard<std::mutex> lk(g_ctx.mtx);
            g_ctx.snap = std::move(snapshot);
            g_ctx.valid = game_.local_pawn() != 0;
        }

        g_mouse.set_local_pawn(game_.local_pawn());

        bool aim = false, trig = false, rcs_on = false;
        float fov = 12.f, sm = 6.f;
        float rcs_strength = 0.5f;
        {
            std::lock_guard<std::mutex> lk(g_ctx.mtx);
            aim = g_ctx.aim_on;  // menu checkbox and X key are kept in sync
            trig = g_ctx.trigger_on;
            // Mutually exclusive: aim_recoil (punch inside the aimbot) and the
            // standalone RCS both push the mouse down by the same punch - never
            // run both at once (double compensation drags shots low).
            rcs_on = g_ctx.rcs_on && !g_ctx.aim_recoil;
            rcs_strength = g_ctx.rcs_strength;
            fov = g_ctx.aim_fov;
            sm = g_ctx.aim_smooth;
        }
        const bool steered = aimbot_.run(aim, fov, sm);
        {
            std::lock_guard<std::mutex> lk(g_ctx.mtx);
            g_ctx.aim_active = steered;  // screen hint: aiming right now
        }
        bool fire_now = false;

        if (steered)
            Logger::instance().log("aimbot: steered (aim=%d)\n", aim ? 1 : 0);
        static bool last_aim = false;
        if (aim != last_aim) {
            last_aim = aim;
            Logger::instance().log("aim: %s (aim_on=%d aim_toggle=%d)\n", aim ? "ACTIVE" : "off",
                                   g_ctx.aim_on ? 1 : 0, g_ctx.aim_toggle ? 1 : 0);
        }
        rcs_.run(rcs_on, rcs_strength);       // recoil control (independent)
        trigger_.run(trig, fire_now);         // schedule (delayed shot)
        trigger_.run_shoot();                 // drive the button

        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    Logger::instance().log("data_thread: exited\n");
}
