#pragma once

#include <cstdint>

#include <sys/mman.h>

#include "logger.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

// ----------------------------------------------------------------------------
// Minimal x86-64 inline-hook helper for the Vulkan loader dispatch stubs.
//
// Every loader stub begins with `endbr64` (CET marker, 4 bytes). We preserve
// it and patch from byte 4 onward: this keeps indirect calls IBT-compliant
// while still redirecting execution to our detour.
//
// A trampoline replays the stolen instructions and jumps back into the
// original function right after the patched region.
// ----------------------------------------------------------------------------

namespace hook64 {

// One inline hook. Owns the trampoline and the original bytes so it can be
// installed and later cleanly restored.
class Hook {
public:
    // Installs a jump at fn+4 -> detour, with a trampoline that replays the
    // stolen bytes and jumps back. Requires fn to start with endbr64.
    bool install(void* fn, void* detour);
    // Restores the original bytes and frees the trampoline. Safe to call even
    // if the hook was never installed.
    void uninstall();

    bool installed() const { return fn_ != nullptr; }
    void* target() const { return fn_; }
    void* trampoline() const { return trampoline_; }
    int stolen_len() const { return stolen_len_; }

    // --- x86-64 instruction decoder (stateless, exposed for tests) ------------
    // Length of the ModRM [+SIB] [+disp] + immediate part of an instruction,
    // EXCLUDING the opcode and any REX prefix. `modrm_off` is the index of the
    // ModRM byte within `p`.
    static int modrm_len(const std::uint8_t* p, int modrm_off, int imm_bytes);
    static int modrm_imm_bytes(std::uint8_t opcode);
    // Instruction length decoder (covers the loader stub opcodes and common
    // prologue code). Returns 0 when the instruction is unrecognized.
    static int insn_len(const std::uint8_t* p);

private:
    // Writes `jmp qword ptr [rip+0]` at `at`, followed by the absolute 64-bit
    // target (works across arbitrary distances, unlike a rel32 jump).
    static void emit_indirect_jmp(std::uint8_t* at, const void* target);

    void* fn_ = nullptr;
    void* trampoline_ = nullptr;
    int stolen_len_ = 0;
    bool has_endbr_ = false;
    std::uint8_t orig_[24] = {};  // original bytes (stolen region, incl. endbr64)
};

}  // namespace hook64
