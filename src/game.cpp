#include "game.h"
#include "cfg.h"

#include "offsets.h"
#include "process.h"

#include <algorithm>
#include <cmath>

namespace {

// CEntityHandle: low 15 bits = entity index, high bits = serial.
int handle_index(std::uint32_t handle) {
    return static_cast<int>(handle & 0x7FFFu);
}

}  // namespace

bool Game::attach(const Memory& mem, const patterns::Resolved& off) {
    if (!mem.attached() || !off.ok) return false;
    const auto base = module_base(mem.pid(), offsets::CLIENT_MODULE);
    if (!base) return false;
    client_base_ = *base;
    off_ = off;
    return true;
}

void Game::update(Memory& mem) {
    if (!attached()) return;

    // --- entity list (Osiris: *gameEntitySystem + entityListOffset) ----------
    entity_list_ = 0;
    if (off_.gameEntitySystem) {
        if (const auto gs = mem.read<std::uintptr_t>(off_.gameEntitySystem); gs && *gs)
            entity_list_ = *gs + static_cast<std::uintptr_t>(off_.entityListOffset);
    }

    // --- local pawn ----------------------------------------------------------
    // Preferred: controller -> m_hPawn handle -> entity identity -> instance.
    local_pawn_ = 0;
    if (off_.localPlayerController) {
        if (const auto ctrl = mem.read<std::uintptr_t>(off_.localPlayerController);
            ctrl && *ctrl) {
            if (const auto handle = mem.read<std::uint32_t>(*ctrl + off_.m_hPawn);
                handle && *handle) {
                local_pawn_ = entity_by_index_impl(mem, handle_index(*handle));
            }
        }
    }
    // Fallback: dwLocalPlayerPawn global (best-effort; only used if the
    // controller path is unavailable).
    if (!local_pawn_)
        local_pawn_ = mem.read<std::uintptr_t>(client_base_ + cs2cfg().offsets.dwLocalPlayerPawn)
                          .value_or(0);

    // --- view matrix / angles ------------------------------------------------
    if (off_.viewMatrix)
        mem.read(off_.viewMatrix, view_matrix_, sizeof(view_matrix_));
    if (view_angle_source_)
        view_angles_ = mem.read<Vector3>(view_angle_source_).value_or({});
    else
        view_angles_ =
            mem.read<Vector3>(client_base_ + offsets::dwViewAngles).value_or({});

    players_.clear();
    local_ = Player{};

    if (!local_pawn_) return;
    read_player(mem, local_pawn_, true, local_);
    if (!entity_list_) return;

    // --- enemies (Osiris chunk traversal over the networkable chunks) --------
    for (int i = 0; i < offsets::ENTITY_LIST_CHUNK_COUNT * offsets::ENTITY_LIST_CHUNK_SIZE; ++i) {
        const std::uintptr_t ent = entity_by_index_impl(mem, i);
        if (!ent || ent == local_pawn_) continue;

        Player p;
        read_player(mem, ent, false, p);
        if (!p.valid || !p.alive) continue;
        if (p.team != 2 && p.team != 3) continue;  // skip spectators etc.
        project_radar(local_, p);
        players_.push_back(p);
    }

    std::sort(players_.begin(), players_.end(),
              [](const Player& a, const Player& b) { return a.distance_m < b.distance_m; });
}

Snapshot Game::snapshot() const {
    Snapshot s;
    s.local = local_;
    s.players = players_;
    s.view_angles = view_angles_;
    std::copy(std::begin(view_matrix_), std::end(view_matrix_), s.view_matrix);
    s.connected = attached();
    s.valid = local_pawn_ != 0;
    return s;
}

void Game::read_player(const Memory& mem, std::uintptr_t addr,
                       bool is_local, Player& out) const {
    out = Player{};
    out.address = addr;
    out.local = is_local;

    const auto health = mem.read<int>(addr + off_.m_iHealth);
    // m_lifeState is a single byte (the pattern is `movzx edx, byte ptr [...]
    // `); reading it as int32 would pick up the following byte (0x100).
    const auto life = mem.read<std::uint8_t>(addr + off_.m_lifeState);
    const auto team = mem.read<int>(addr + off_.m_iTeamNum);
    const auto armor = mem.read<int>(addr + cs2cfg().offsets.m_ArmorValue);
    if (!health || !life || !team || !armor) return;

    out.health = *health;
    out.armor = *armor;
    out.team = *team;
    out.alive = *life == 0 && *health > 0 && *health <= 100;
    out.scoped = mem.read<bool>(addr + off_.m_bIsScoped).value_or(false);
    out.flash_alpha = mem.read<float>(addr + cs2cfg().offsets.m_flFlashOverlayAlpha).value_or(0.f);

    // Feet origin: CGameSceneNode::m_vecOrigin (engine layout, update-proof).
    if (const auto scene = mem.read<std::uintptr_t>(addr + off_.m_pGameSceneNode);
        scene && *scene)
        out.feet = mem.read<Vector3>(*scene + offsets::SCENE_NODE_ORIGIN).value_or({});
    out.velocity = mem.read<Vector3>(addr + cs2cfg().offsets.m_vecVelocity).value_or({});

    // Head = head_0 bone (index from config; 7 on current Linux build).
    read_skeleton(mem, addr, out);
    if (out.bone_pos.size() > static_cast<std::size_t>(cs2cfg().offsets.boneHeadIndex) &&
        out.bone_pos[cs2cfg().offsets.boneHeadIndex].Length() > 0.001f) {
        out.head = out.bone_pos[cs2cfg().offsets.boneHeadIndex];
    } else {
        out.head = out.feet + Vector3{0.f, 0.f, 72.f};  // fallback: ~standing height
    }
    out.valid = true;
}

bool Game::read_bone(const Memory& mem, std::uintptr_t pawn, int bone, Vector3& out) const {
    const auto scene = mem.read<std::uintptr_t>(pawn + cs2cfg().offsets.m_pGameSceneNode);
    if (!scene || !*scene) return false;
    const auto bone_array =
        mem.read<std::uintptr_t>(*scene + cs2cfg().offsets.m_modelState +
                                 cs2cfg().offsets.boneStateData);
    if (!bone_array || !*bone_array) return false;
    const auto v = mem.read<Vector3>(
        *bone_array + static_cast<std::uintptr_t>(bone) * cs2cfg().offsets.boneElementSize);
    if (!v) return false;
    out = *v;
    return true;
}

void Game::read_skeleton(const Memory& mem, std::uintptr_t pawn, Player& out) const {
    // Reads the per-frame bone transforms (CUtlVector<CBoneStateData> embedded
    // inside CModelState at scene_node + m_modelState; boneStateData is the
    // vector's data pointer) and the static bone table (CModel).
    out.bone_parents.clear();
    out.bone_pos.clear();
    out.bone_flags.clear();

    const auto scene = mem.read<std::uintptr_t>(pawn + cs2cfg().offsets.m_pGameSceneNode);
    if (!scene || !*scene) return;

    const auto bsd =
        mem.read<std::uintptr_t>(*scene + cs2cfg().offsets.m_modelState +
                                 cs2cfg().offsets.boneStateData);
    if (!bsd || !*bsd) return;

    // Model handle: CModelState + m_hModel -> deref once -> CModel.
    const auto hm = mem.read<std::uintptr_t>(*scene + cs2cfg().offsets.m_modelState +
                                             cs2cfg().offsets.m_hModel);
    const std::uintptr_t cmodel = hm ? mem.read<std::uintptr_t>(*hm).value_or(0) : 0;
    if (!cmodel) return;

    const auto count = mem.read<int>(cmodel + cs2cfg().offsets.boneCount).value_or(0);
    if (count <= 0 || count > 2000) return;
    const auto names = mem.read<std::uintptr_t>(cmodel + cs2cfg().offsets.boneNames).value_or(0);
    const auto parents = mem.read<std::uintptr_t>(cmodel + cs2cfg().offsets.boneParents).value_or(0);
    const auto flags = mem.read<std::uintptr_t>(cmodel + cs2cfg().offsets.boneFlags).value_or(0);
    if (!names || !parents || !flags) return;

    out.bone_parents.reserve(count);
    out.bone_pos.reserve(count);
    out.bone_flags.reserve(count);
    for (int i = 0; i < count; ++i) {
        const auto p = mem.read<Vector3>(*bsd + static_cast<std::uintptr_t>(i) *
                                                   cs2cfg().offsets.boneElementSize);
        if (p) out.bone_pos.push_back(*p);
        else out.bone_pos.emplace_back();  // keep 1:1 with parents
        out.bone_parents.push_back(mem.read<std::uint16_t>(parents + i * 2).value_or(0xFFFF));
        out.bone_flags.push_back(mem.read<std::uint32_t>(flags + i * 4).value_or(0));
    }
}

void Game::project_radar(const Player& local, Player& p) const {
    // Relative world offset, Source units -> meters.
    const Vector3 rel = (p.feet - local.feet) / kUnitsPerMeter;
    // Yaw 0 faces +x; positive yaw turns left (toward +y).
    const float yaw = view_angles_.y * kDeg2Rad;
    const float fwd_x = std::cos(yaw), fwd_y = std::sin(yaw);
    const float right_x = -fwd_y, right_y = fwd_x;
    p.radar_xy.x = rel.x * right_x + rel.y * right_y;  // + = right
    p.radar_xy.y = rel.x * fwd_x + rel.y * fwd_y;      // + = forward
    p.distance_m = rel.Length();
}

Vector3 Game::local_eye(const Memory& mem) const {
    if (!local_pawn_) return {};
    Vector3 off = mem.read<Vector3>(local_pawn_ + cs2cfg().offsets.m_vecViewOffset).value_or({});
    // Sanity guard: the eye offset should be roughly head-height above origin.
    if (off.z < 0.f || off.z > 300.f || std::isnan(off.x) || std::isnan(off.y) ||
        std::isnan(off.z))
        off = {0.f, 0.f, 64.f};
    return local_.feet + off;
}

bool Game::set_view_angles(const Memory& mem, const Vector3& ang) const {
    if (angles_override_) {
        angles_override_(ang, true);
        return true;
    }
    return mem.write<Vector3>(client_base_ + offsets::dwViewAngles, ang);
}

std::uintptr_t Game::entity_list() const {
    if (!attached()) return 0;
    return entity_list_;
}

std::uintptr_t Game::entity_by_index_impl(const Memory& mem, int index) const {
    if (!entity_list_ || index < 0) return 0;
    const int chunk = index / offsets::ENTITY_LIST_CHUNK_SIZE;
    const int in_chunk = index % offsets::ENTITY_LIST_CHUNK_SIZE;
    if (chunk >= offsets::ENTITY_LIST_CHUNK_COUNT) return 0;

    const auto chunk_ptr =
        mem.read<std::uintptr_t>(entity_list_ + offsets::ENTITY_LIST_CHUNKS_OFFSET +
                                 static_cast<std::uintptr_t>(chunk) * 8);
    if (!chunk_ptr || !*chunk_ptr) return 0;

    const std::uintptr_t identity =
        *chunk_ptr + static_cast<std::uintptr_t>(in_chunk) * offsets::ENTITY_IDENTITY_SIZE;
    return mem.read<std::uintptr_t>(identity + offsets::ENTITY_IDENTITY_ENTITY).value_or(0);
}

std::uintptr_t Game::entity_by_index(const Memory& mem, int index) const {
    return entity_by_index_impl(mem, index);
}

int Game::entity_team(const Memory& mem, std::uintptr_t ent) const {
    return mem.read<int>(ent + off_.m_iTeamNum).value_or(0);
}

int Game::entity_health(const Memory& mem, std::uintptr_t ent) const {
    return mem.read<int>(ent + off_.m_iHealth).value_or(0);
}
