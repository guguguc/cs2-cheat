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
};

struct OffsetCfg {
    std::uintptr_t dwCSGOInput        = 0x4576E98;
    std::uintptr_t viewAngleOffset    = 0x9C;
    std::uintptr_t m_vecViewOffset    = 0xDF8;
    std::uintptr_t m_ArmorValue       = 0x2B34;
    std::uintptr_t m_iIDEntIndex      = 0x42C4;
    std::uintptr_t m_pAimPunchServices = 0x1440;   // C_CSPlayerPawn -> CCSPlayer_AimPunchServices
    std::uintptr_t aimPunchCache      = 0x88;      // CCSPlayer_AimPunchServices::m_aimPunchCache (CUtlVector<QAngle>)
    std::uintptr_t m_iShotsFired      = 0x2B1C;    // C_CSPlayerPawn::m_iShotsFired
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
