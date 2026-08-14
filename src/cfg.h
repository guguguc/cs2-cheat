#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct PatternCfg {
    std::string name;
    std::string pattern;
    int off = 0;
    std::string op;  // "read" | "abs4" | "abs5"
    int size = 4;    // Read op: 1 (disp8) or 4 (disp32)
    int len = 4;     // Abs op: instruction length after the disp (RIP-relative)
};

struct OffsetCfg {
    std::uintptr_t dwCSGOInput        = 0x4576E98;
    std::uintptr_t viewAngleOffset    = 0x9C;
    std::uintptr_t m_vecViewOffset    = 0xDF8;
    std::uintptr_t m_ArmorValue       = 0x2B34;
    std::uintptr_t m_iIDEntIndex      = 0x42C4;
    std::uintptr_t m_pAimPunchServices = 0x1440;   // C_CSPlayerPawn -> CCSPlayer_AimPunchServices
    std::uintptr_t aimPunchCache      = 0x88;      // CCSPlayer_AimPunchServices::m_aimPunchCache (CUtlVector<QAngle>)
    std::uintptr_t m_iShotsFired      = 0x2B14;    // C_CSPlayerPawn::m_iShotsFired (dumper 2026-08-13)
    std::uintptr_t m_flFOVSensitivityAdjust = 0x1338; // C_BasePlayerPawn::m_flFOVSensitivityAdjust (f32)
    std::uintptr_t m_pWeaponServices  = 0x1190;    // C_BasePlayerPawn::m_pWeaponServices
    std::uintptr_t m_hActiveWeapon    = 0x60;      // CPlayer_WeaponServices::m_hActiveWeapon (handle)
    std::uintptr_t m_designerName     = 0x20;      // CEntityIdentity::m_designerName (PtrCStr)
    // ---- bone chain (Linux, verified by scripts/scan_bones.py) -------------
    std::uintptr_t m_pGameSceneNode   = 0x4A0;     // C_BaseEntity::m_pGameSceneNode
    std::uintptr_t m_modelState       = 0x140;     // CSkeletonInstance::m_modelState (EMBEDDED CModelState)
    std::uintptr_t boneStateData      = 0x80;      // CModelState::bone_state_data (CUtlVector<CBoneStateData>)
    std::uintptr_t m_hModel           = 0xA0;      // CModelState::m_hModel (handle; deref once)
    std::uintptr_t boneCount          = 0x160;     // CModel::bone_count
    std::uintptr_t boneNames          = 0x168;     // CModel::bone_names (PtrCStr array)
    std::uintptr_t boneParents        = 0x180;     // CModel::bone_parents (u16 array)
    std::uintptr_t boneFlags          = 0x1B0;     // CModel::bone_flags (u32 array)
    std::uintptr_t boneElementSize    = 0x20;      // CBoneStateData stride
    int            boneHeadIndex      = 7;         // head_0
    int            boneNeckIndex      = 6;         // neck_0
    std::uintptr_t m_vecVelocity      = 0x5A0;
    std::uintptr_t m_flFlashOverlayAlpha = 0x13A4;
    std::uintptr_t dwLocalPlayerPawn  = 0x4802768;
    float sensitivity                 = 2.5f;
};

struct Config {
    std::vector<PatternCfg> patterns;
    std::vector<std::string> required;
    OffsetCfg offsets;
    bool loaded = false;
};

// Loads config from CS2_CONFIG (default: /home/gugugu/Repo/cs2-cheat/config/
// cs2_config.json). On failure returns defaults so the cheat still works.
const Config& cs2cfg();
