#pragma once
#include <cstdint>

// ============================================================================
//  CS2 (native Linux build) offsets.
//
//  IMPORTANT: the offsets the cheat actually depends on (entity system, entity
//  list, m_iHealth/m_iTeamNum/m_lifeState/m_pGameSceneNode, the controller's
//  pawn handle, dwLocalPlayerController, dwViewMatrix) are resolved at runtime
//  from the game's machine code via byte-pattern scanning (src/patterns.cpp),
//  ported from danielkrupinski/Osiris' Linux patterns. They survive game
//  updates without manual changes.
//
//  What remains here:
//    * engine struct layouts (CEntityIdentity / CConcreteEntityList /
//      CGameSceneNode / CSkeletonInstance) — hardcoded the same way Osiris
//      hardcodes them; these layouts do not move between updates.
//    * a few auxiliary schema fields (armor, velocity, flash, eye offset,
//      name, crosshair entity index) plus a couple of globals that have no
//      Osiris pattern. These are best-effort values and only affect cosmetic
//      features (armor/flash display, player names) or minor aim precision.
// ============================================================================
namespace offsets {

// ---- globals without an Osiris pattern (best-effort, from a local dump) -----
inline constexpr std::uintptr_t dwViewAngles = 0x45773E0;     // QAngle (write-to for aim)
inline constexpr std::uintptr_t dwLocalPlayerPawn = 0x4802768; // C_CSPlayerPawn* (fallback only)
inline constexpr std::uintptr_t dwCSGOInput = 0x4576E98;

// ---- auxiliary schema fields (best-effort; cosmetic / minor features) -------
inline constexpr std::uintptr_t m_ArmorValue = 0x2B34;          // armor display
inline constexpr std::uintptr_t m_iIDEntIndex = 0x42C4;         // triggerbot crosshair entity
inline constexpr std::uintptr_t m_vecVelocity = 0x5A0;          // radar velocity
inline constexpr std::uintptr_t m_vecViewOffset = 0xDF8;        // eye offset (aim precision)
inline constexpr std::uintptr_t m_flFlashOverlayAlpha = 0x13A4; // flash indicator

// ---- engine struct layouts (hardcoded, like Osiris does) --------------------
// CEntityIdentity (sizeof == 0x70, verified by Osiris' static_assert)
inline constexpr std::uintptr_t ENTITY_IDENTITY_SIZE    = 0x70;
inline constexpr std::uintptr_t ENTITY_IDENTITY_ENTITY  = 0x00;  // CEntityInstance*
inline constexpr std::uintptr_t ENTITY_IDENTITY_CLASS   = 0x08;  // CEntityClass*
inline constexpr std::uintptr_t ENTITY_IDENTITY_HANDLE  = 0x10;  // CEntityHandle

// CConcreteEntityList: pointer array of CEntityIdentity[512] chunks
inline constexpr std::uintptr_t ENTITY_LIST_CHUNKS_OFFSET = 0x00;
inline constexpr int            ENTITY_LIST_CHUNK_COUNT   = 32;    // networkable chunks
inline constexpr int            ENTITY_LIST_CHUNK_SIZE    = 512;   // identities per chunk

// CGameSceneNode / CSkeletonInstance / CModelState
inline constexpr std::uintptr_t SCENE_NODE_ORIGIN = 0x80;     // CGameSceneNode::m_vecOrigin
inline constexpr std::uintptr_t m_modelState     = 0x140;     // CSkeletonInstance::m_modelState
inline constexpr std::uintptr_t BONE_ARRAY_OFFSET = 0x80;     // bone-matrix ptr in CModelState

// ---- bones (CS2 skeleton) ---------------------------------------------------
namespace bones {
inline constexpr int head  = 6;
inline constexpr int neck  = 5;
inline constexpr int chest = 4;
}

// ---- process / module -------------------------------------------------------
inline constexpr const char* PROCESS_NAME  = "cs2";          // native Linux binary
inline constexpr const char* CLIENT_MODULE = "libclient.so";

}  // namespace offsets
