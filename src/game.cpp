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

bool Game::attach(const patterns::Resolved& off) {
    if (!mem_.attached() || !off.ok) return false;
    const auto base = module_base(mem_.pid(), offsets::CLIENT_MODULE);
    if (!base) return false;
    client_base_ = *base;
    off_ = off;
    return true;
}

void Game::update() {
    if (!attached()) return;

    // --- entity list (Osiris: *gameEntitySystem + entityListOffset) ----------
    entity_list_ = 0;
    if (off_.gameEntitySystem) {
        if (const auto gs = mem_.read<std::uintptr_t>(off_.gameEntitySystem); gs && *gs)
            entity_list_ = *gs + static_cast<std::uintptr_t>(off_.entityListOffset);
    }

    // --- local pawn ----------------------------------------------------------
    // Preferred: controller -> m_hPawn handle -> entity identity -> instance.
    local_pawn_ = 0;
    if (off_.localPlayerController) {
        if (const auto ctrl = mem_.read<std::uintptr_t>(off_.localPlayerController);
            ctrl && *ctrl) {
            if (const auto handle = mem_.read<std::uint32_t>(*ctrl + off_.m_hPawn);
                handle && *handle) {
                local_pawn_ = entity_by_index_impl(handle_index(*handle));
            }
        }
    }
    // Fallback: dwLocalPlayerPawn global (best-effort; only used if the
    // controller path is unavailable).
    if (!local_pawn_)
        local_pawn_ = mem_.read<std::uintptr_t>(client_base_ + Config::instance().offsets.dwLocalPlayerPawn)
                          .value_or(0);

    // --- view matrix / angles ------------------------------------------------
    if (off_.viewMatrix)
        mem_.read(off_.viewMatrix, view_matrix_, sizeof(view_matrix_));
    if (view_angle_source_)
        view_angles_ = mem_.read<Vector3>(view_angle_source_).value_or({});
    else
        view_angles_ =
            mem_.read<Vector3>(client_base_ + offsets::dwViewAngles).value_or({});

    players_.clear();
    local_ = Player{};

    if (!local_pawn_) return;
    read_player(local_pawn_, true, local_);
    if (!entity_list_) return;

    // --- enemies (Osiris chunk traversal over the networkable chunks) --------
    for (int i = 0; i < offsets::ENTITY_LIST_CHUNK_COUNT * offsets::ENTITY_LIST_CHUNK_SIZE; ++i) {
        const std::uintptr_t ent = entity_by_index_impl(i);
        if (!ent || ent == local_pawn_) continue;

        Player p;
        read_player(ent, false, p);
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

void Game::read_player(std::uintptr_t addr, bool is_local, Player& out) const {
    out = Player{};
    out.address = addr;
    out.local = is_local;

    const auto health = mem_.read<int>(addr + off_.m_iHealth);
    // m_lifeState is a single byte (the pattern is `movzx edx, byte ptr [...]
    // `); reading it as int32 would pick up the following byte (0x100).
    const auto life = mem_.read<std::uint8_t>(addr + off_.m_lifeState);
    const auto team = mem_.read<int>(addr + off_.m_iTeamNum);
    const auto armor = mem_.read<int>(addr + Config::instance().offsets.m_ArmorValue);
    if (!health || !life || !team || !armor) return;

    out.health = *health;
    out.armor = *armor;
    out.team = *team;
    out.alive = *life == 0 && *health > 0 && *health <= 100;
    out.scoped = mem_.read<bool>(addr + off_.m_bIsScoped).value_or(false);
    out.flash_alpha = mem_.read<float>(addr + Config::instance().offsets.m_flFlashOverlayAlpha).value_or(0.f);

    // Feet origin: CGameSceneNode::m_vecOrigin (engine layout, update-proof).
    if (const auto scene = mem_.read<std::uintptr_t>(addr + off_.m_pGameSceneNode);
        scene && *scene)
        out.feet = mem_.read<Vector3>(*scene + offsets::SCENE_NODE_ORIGIN).value_or({});
    out.velocity = mem_.read<Vector3>(addr + Config::instance().offsets.m_vecVelocity).value_or({});

    // Head = head_0 bone (index from config; 7 on current Linux build).
    read_skeleton(addr, out);
    if (out.bone_pos.size() > static_cast<std::size_t>(Config::instance().offsets.boneHeadIndex) &&
        out.bone_pos[Config::instance().offsets.boneHeadIndex].Length() > 0.001f) {
        out.head = out.bone_pos[Config::instance().offsets.boneHeadIndex];
    } else {
        out.head = out.feet + Vector3{0.f, 0.f, 72.f};  // fallback: ~standing height
    }
    out.valid = true;
}

bool Game::read_bone(std::uintptr_t pawn, int bone, Vector3& out) const {
    const auto scene = mem_.read<std::uintptr_t>(pawn + Config::instance().offsets.m_pGameSceneNode);
    if (!scene || !*scene) return false;
    const auto bone_array =
        mem_.read<std::uintptr_t>(*scene + Config::instance().offsets.m_modelState +
                                  Config::instance().offsets.boneStateData);
    if (!bone_array || !*bone_array) return false;
    const auto v = mem_.read<Vector3>(
        *bone_array + static_cast<std::uintptr_t>(bone) * Config::instance().offsets.boneElementSize);
    if (!v) return false;
    out = *v;
    return true;
}

void Game::read_skeleton(std::uintptr_t pawn, Player& out) const {
    // Reads the per-frame bone transforms (CUtlVector<CBoneStateData> embedded
    // inside CModelState at scene_node + m_modelState; boneStateData is the
    // vector's data pointer) and the static bone table (CModel).
    out.bone_parents.clear();
    out.bone_pos.clear();
    out.bone_flags.clear();

    const auto scene = mem_.read<std::uintptr_t>(pawn + Config::instance().offsets.m_pGameSceneNode);
    if (!scene || !*scene) return;

    const auto bsd =
        mem_.read<std::uintptr_t>(*scene + Config::instance().offsets.m_modelState +
                                  Config::instance().offsets.boneStateData);
    if (!bsd || !*bsd) return;

    // Model handle: CModelState + m_hModel -> deref once -> CModel.
    const auto hm = mem_.read<std::uintptr_t>(*scene + Config::instance().offsets.m_modelState +
                                              Config::instance().offsets.m_hModel);
    const std::uintptr_t cmodel = hm ? mem_.read<std::uintptr_t>(*hm).value_or(0) : 0;
    if (!cmodel) return;

    const auto count = mem_.read<int>(cmodel + Config::instance().offsets.boneCount).value_or(0);
    if (count <= 0 || count > 2000) return;
    const auto names = mem_.read<std::uintptr_t>(cmodel + Config::instance().offsets.boneNames).value_or(0);
    const auto parents = mem_.read<std::uintptr_t>(cmodel + Config::instance().offsets.boneParents).value_or(0);
    const auto flags = mem_.read<std::uintptr_t>(cmodel + Config::instance().offsets.boneFlags).value_or(0);
    if (!names || !parents || !flags) return;

    out.bone_parents.reserve(count);
    out.bone_pos.reserve(count);
    out.bone_flags.reserve(count);
    for (int i = 0; i < count; ++i) {
        const auto p = mem_.read<Vector3>(*bsd + static_cast<std::uintptr_t>(i) *
                                                   Config::instance().offsets.boneElementSize);
        if (p) out.bone_pos.push_back(*p);
        else out.bone_pos.emplace_back();  // keep 1:1 with parents
        out.bone_parents.push_back(mem_.read<std::uint16_t>(parents + i * 2).value_or(0xFFFF));
        out.bone_flags.push_back(mem_.read<std::uint32_t>(flags + i * 4).value_or(0));
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

Vector3 Game::local_eye() const {
    if (!local_pawn_) return {};
    Vector3 off = mem_.read<Vector3>(local_pawn_ + Config::instance().offsets.m_vecViewOffset).value_or({});
    // Sanity guard: the eye offset should be roughly head-height above origin.
    if (off.z < 0.f || off.z > 300.f || std::isnan(off.x) || std::isnan(off.y) ||
        std::isnan(off.z))
        off = {0.f, 0.f, 64.f};
    return local_.feet + off;
}

bool Game::set_view_angles(const Vector3& ang) const {
    if (angles_override_) {
        angles_override_(ang, true);
        return true;
    }
    return mem_.write<Vector3>(client_base_ + offsets::dwViewAngles, ang);
}

std::uintptr_t Game::entity_list() const {
    if (!attached()) return 0;
    return entity_list_;
}

std::string Game::weapon_name() const {
    // pawn -> m_pWeaponServices -> m_hActiveWeapon (handle)
    const std::uintptr_t pawn = local_pawn_;
    if (!pawn) return {};
    const auto ws = mem_.read<std::uintptr_t>(pawn + Config::instance().offsets.m_pWeaponServices);
    if (!ws || !*ws) return {};
    const auto handle = mem_.read<int>(*ws + Config::instance().offsets.m_hActiveWeapon);
    if (!handle || *handle <= 0) return {};
    // handle -> entity (CEntityInstance), +0x10 -> CEntityIdentity, +0x20 -> m_designerName
    const std::uintptr_t ent = entity_by_handle(*handle);
    if (!ent) return {};
    const auto identity = mem_.read<std::uintptr_t>(ent + 0x10);
    if (!identity || !*identity) return {};
    const auto name_ptr = mem_.read<std::uintptr_t>(*identity + Config::instance().offsets.m_designerName);
    if (!name_ptr || !*name_ptr) return {};
    char buf[64] = {0};
    mem_.read(*name_ptr, buf, sizeof(buf) - 1);
    std::string name(buf);
    // strip "weapon_" prefix (deadlocked does name.replace("weapon_", ""))
    if (name.rfind("weapon_", 0) == 0) name = name.substr(7);
    return name;
}

std::uintptr_t Game::entity_by_index_impl(int index) const {
    if (!entity_list_ || index < 0) return 0;
    const int chunk = index / offsets::ENTITY_LIST_CHUNK_SIZE;
    const int in_chunk = index % offsets::ENTITY_LIST_CHUNK_SIZE;
    if (chunk >= offsets::ENTITY_LIST_CHUNK_COUNT) return 0;

    const auto chunk_ptr =
        mem_.read<std::uintptr_t>(entity_list_ + offsets::ENTITY_LIST_CHUNKS_OFFSET +
                                  static_cast<std::uintptr_t>(chunk) * 8);
    if (!chunk_ptr || !*chunk_ptr) return 0;

    const std::uintptr_t identity =
        *chunk_ptr + static_cast<std::uintptr_t>(in_chunk) * offsets::ENTITY_IDENTITY_SIZE;
    return mem_.read<std::uintptr_t>(identity + offsets::ENTITY_IDENTITY_ENTITY).value_or(0);
}

std::uintptr_t Game::entity_by_index(int index) const {
    return entity_by_index_impl(index);
}

int Game::entity_team(std::uintptr_t ent) const {
    return mem_.read<int>(ent + off_.m_iTeamNum).value_or(0);
}

int Game::entity_health(std::uintptr_t ent) const {
    return mem_.read<int>(ent + off_.m_iHealth).value_or(0);
}
