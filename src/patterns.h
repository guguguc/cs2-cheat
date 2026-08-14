#pragma once

#include <cstdint>

// ============================================================================
//  Runtime offset resolver — ported from danielkrupinski/Osiris (Linux).
//
//  Osiris never hardcodes offsets: on every launch it scans the executable
//  section of the game's client library for byte signatures and extracts the
//  field offsets / global addresses directly from the machine code. This is
//  what makes it survive game updates without any manual offset updates.
//
//  Two resolution operations are used (see Osiris PatternSearchResult):
//    Read : read the raw displacement at (match + add) -> the field offset.
//    Abs  : RIP-relative address -> match + add + nextInstrLen + disp32.
// ============================================================================
namespace patterns {

enum class Op : std::uint8_t { None, Read, Abs4, Abs5 };

struct Pattern {
    const char* hex;   // byte signature, "? " = wildcard, e.g. "C7 87 ? ? ? ?"
    int add = 0;       // byte offset within the match where the value lives
    Op op = Op::None;
    int read_size = 4; // Read op: 1 (disp8) or 4 (disp32) bytes
    int len = 4;       // Abs op: instruction length after the disp (RIP-relative)
};

// Everything the cheat needs, resolved from the live binary.
struct Resolved {
    // --- globals (absolute addresses in the game process) ------------------
    std::uintptr_t gameEntitySystem = 0;       // CGameEntitySystem** (slot)
    std::uintptr_t localPlayerController = 0;  // CCSPlayerController** (slot)
    std::uintptr_t globalVars = 0;             // GlobalVars** (slot)
    std::uintptr_t viewMatrix = 0;             // VMatrix* (direct)
    std::uintptr_t viewRender = 0;             // CViewRender** (slot)
    std::uintptr_t vphysWorld = 0;             // IVPhysicsWorld** (slot, BVH LOS)

    // --- entity system ------------------------------------------------------
    int entityListOffset = 0;  // *gameEntitySystem + this -> CConcreteEntityList

    // --- C_BaseEntity fields ------------------------------------------------
    int m_pGameSceneNode = 0;
    int m_iHealth = 0;
    int m_lifeState = 0;
    int m_iTeamNum = 0;

    // --- CCSPlayerController field ------------------------------------------
    int m_hPawn = 0;  // pawn handle; local pawn = identity(handle)->entity

    // --- C_CSPlayerPawn fields ----------------------------------------------
    int m_pWeaponServices = 0;
    int m_bIsScoped = 0;

    bool ok = false;  // true only when every required pattern was found
};

// Scans the executable section of libclient.so in `pid` and fills `out`.
// Returns false (and logs which pattern) if any required pattern is missing.
bool resolve(int pid, Resolved& out);

}  // namespace patterns
