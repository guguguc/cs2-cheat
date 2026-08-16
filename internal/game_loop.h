#pragma once

#include "aimbot.h"
#include "game.h"
#include "memory.h"
#include "patterns.h"
#include "rcs.h"
#include "trigger.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>

// Owns the data thread: resolves offsets, attaches to the game, wires up the
// uinput input path, then runs the per-frame update loop (game state, BVH
// visibility, aimbot / triggerbot / RCS). Stopped by clearing the `run` flag.
class GameLoop {
public:
    explicit GameLoop(std::atomic<bool>& run);

    // Pattern-scan, attach to the game and configure the input path. Returns
    // false when the offsets could not be resolved or the game could not be
    // attached (the caller should not call run()).
    bool init();
    // Per-frame update loop; blocks until `run` is cleared.
    void run();

private:
    void check_bvh(const patterns::Resolved& off);
    std::uintptr_t find_sensitivity_convar() const;

    std::atomic<bool>& run_;
    Memory mem_;
    patterns::OffsetResolver offsets_;
    Game game_;
    Aimbot aimbot_;
    Rcs rcs_;
    Triggerbot trigger_;

    std::unordered_map<std::uintptr_t, bool> visibility_cache_;
    std::chrono::steady_clock::time_point next_visibility_update_{};
    std::string last_map_;
    std::chrono::steady_clock::time_point last_bvh_check_{};
};
