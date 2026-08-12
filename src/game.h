#pragma once

#include <functional>

#include "math.h"
#include "memory.h"
#include "patterns.h"

#include <cstdint>
#include <string>
#include <vector>

struct Player {
    std::uintptr_t address = 0;
    bool local = false;
    bool valid = false;
    bool alive = false;
    bool scoped = false;
    int health = 0;
    int armor = 0;
    int team = 0;            // 2 = T, 3 = CT
    float flash_alpha = 0.f; // 0..255
    std::string name;
    Vector3 feet;            // world origin (Source units)
    Vector3 head;            // head bone (Source units)
    Vector3 velocity;
    float distance_m = 0.f;
    // Radar frame: x = right of local, y = forward of local (meters).
    Vector2 radar_xy;
    // Bone skeleton: parent indices + per-bone world positions (Source units).
    std::vector<int> bone_parents;   // parent bone index per bone (0xFFFF = root)
    std::vector<Vector3> bone_pos;   // world position per bone (1:1 with parents)
    std::vector<std::uint32_t> bone_flags;
};

// Immutable per-frame state handed to renderers and the demo feed.
struct Snapshot {
    Player local;
    std::vector<Player> players;  // enemies (and teammates, per config)
    Vector3 view_angles;          // degrees
    float view_matrix[16]{};      // for world-to-screen (X11 overlay)
    bool connected = false;       // process attached (vs. mid-menu / no pawn)
    bool valid = false;
};

class Game {
public:
    // Resolves the libclient.so base address of the attached process.
    bool attach(const Memory& mem, const patterns::Resolved& offsets);
    // Refreshes local pawn, view matrix/angles and the entity list.
    void update(Memory& mem);

    // Internal (injected) build: point the view-angle read/write at the input
    // object's live view-angle slots instead of the stale dwViewAngles global.
    void set_view_angle_source(std::uintptr_t addr) { view_angle_source_ = addr; }
    void set_angles_override(
        std::function<void(const Vector3&, bool)> fn) { angles_override_ = std::move(fn); }

    bool attached() const { return client_base_ != 0; }
    bool offsets_resolved() const { return off_.ok; }
    std::uintptr_t client_base() const { return client_base_; }
    std::uintptr_t local_pawn() const { return local_pawn_; }
    Vector3 view_angles() const { return view_angles_; }
    const Player& local() const { return local_; }
    const std::vector<Player>& players() const { return players_; }
    Snapshot snapshot() const;

    // Aimbot support.
    Vector3 local_eye(const Memory& mem) const;
    bool set_view_angles(const Memory& mem, const Vector3& ang) const;

    // Triggerbot support (resolve an arbitrary entity index).
    std::uintptr_t entity_by_index(const Memory& mem, int index) const;
    int entity_team(const Memory& mem, std::uintptr_t ent) const;
    int entity_health(const Memory& mem, std::uintptr_t ent) const;

private:
    void read_player(const Memory& mem, std::uintptr_t addr,
                     bool is_local, Player& out) const;
    bool read_bone(const Memory& mem, std::uintptr_t pawn, int bone, Vector3& out) const;
    // Reads the full bone skeleton (parents / world positions / flags).
    void read_skeleton(const Memory& mem, std::uintptr_t pawn, Player& out) const;
    void project_radar(const Player& local, Player& p) const;
    // Osiris CConcreteEntityList chunk traversal.
    std::uintptr_t entity_by_index_impl(const Memory& mem, int index) const;
    std::uintptr_t entity_list() const;

    std::uintptr_t client_base_ = 0;
    std::uintptr_t entity_list_ = 0;
    std::uintptr_t local_pawn_ = 0;
    std::uintptr_t view_angle_source_ = 0;
    std::function<void(const Vector3&, bool)> angles_override_;
    patterns::Resolved off_;
    float view_matrix_[16]{};
    Vector3 view_angles_;
    Player local_;
    std::vector<Player> players_;
};
