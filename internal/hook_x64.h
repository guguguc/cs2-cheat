#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <sys/mman.h>

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

// Length of the ModRM [+SIB] [+disp] + immediate part of an instruction,
// EXCLUDING the opcode and any REX prefix. `modrm_off` is the index of the
// ModRM byte within `p`.
inline int modrm_len(const std::uint8_t* p, int modrm_off, int imm_bytes) {
    const std::uint8_t modrm = p[modrm_off];
    const int mod = modrm >> 6, rm = modrm & 7;
    int len = 1 + imm_bytes;  // modrm byte + immediate
    if (mod != 3 && rm == 4) len += 1;  // memory operand with SIB byte
    if (mod == 1) len += 1;
    else if (mod == 2) len += 4;
    else if (mod == 0 && rm == 5) len += 4;       // [rip+disp32] / [disp32]
    else if (mod == 0 && rm == 4) {               // SIB, no disp unless base==5
        if ((p[modrm_off + 1] & 7) == 5) len += 4;  // [sib + disp32]
    }
    return len;
}

inline int modrm_imm_bytes(std::uint8_t opcode) {
    return (opcode == 0x80 || opcode == 0x83 || opcode == 0xc6) ? 1 : 4;
}

// Instruction length decoder (covers the loader stub opcodes and common
// prologue code). Returns 0 when the instruction is unrecognized.
inline int insn_len(const std::uint8_t* p) {
    const std::uint8_t b0 = p[0];
    if (b0 == 0xf3 && p[1] == 0x0f && p[2] == 0x1e && p[3] == 0xfa) return 4;  // endbr64
    if (b0 == 0x90) return 1;  // nop
    if (b0 == 0xc3) return 1;  // ret
    if (b0 == 0x55 || b0 == 0x53 || b0 == 0x57 || b0 == 0x56 || b0 == 0x54) return 1;
    if (b0 == 0x5d || b0 == 0x5b || b0 == 0x5f || b0 == 0x5e || b0 == 0x5c) return 1;
    if (b0 == 0xe8 || b0 == 0xe9) return 5;  // call/jmp rel32
    if (b0 == 0xeb) return 2;                // jmp rel8
    if (b0 >= 0x70 && b0 <= 0x7f) return 2;  // jcc rel8
    if (b0 == 0x0f && p[1] >= 0x80 && p[1] <= 0x8f) return 6;  // jcc rel32

    if (b0 < 0x40)  // add/or/adc/sbb/and/sub/xor/cmp r/m, r (00-3F): modrm, no imm
        return 1 + modrm_len(p, 1, 0);

    if (b0 == 0x41) {  // REX.B prefix
        const std::uint8_t b1 = p[1];
        if (b1 == 0x57 || b1 == 0x56 || b1 == 0x55 || b1 == 0x54) return 2;
        if (b1 == 0x5f || b1 == 0x5e || b1 == 0x5d || b1 == 0x5c) return 2;
        if (b1 == 0x89 || b1 == 0x8b || b1 == 0x85 || b1 == 0x39 || b1 == 0x3b ||
            b1 == 0x2b || b1 == 0x03 || b1 == 0x8d)
            return 2 + modrm_len(p, 2, 0);  // REX + opcode + modrm part
        if (b1 == 0x80 || b1 == 0x83 || b1 == 0x81 || b1 == 0xc7 || b1 == 0xc6)
            return 2 + modrm_len(p, 2, modrm_imm_bytes(b1));
        if (b1 == 0xb8 || b1 == 0xb9 || b1 == 0xba || b1 == 0xbb) return 6;
        if (b1 == 0x0f) return 5;
        return 0;
    }

    if ((b0 & 0xf0) == 0x40) {  // REX prefix
        const std::uint8_t b1 = p[1];
        if (b1 == 0x89 || b1 == 0x8b || b1 == 0x85 || b1 == 0x39 || b1 == 0x3b ||
            b1 == 0x2b || b1 == 0x03 || b1 == 0x8d)
            return 2 + modrm_len(p, 2, 0);
        if (b1 == 0x80 || b1 == 0x83 || b1 == 0x81 || b1 == 0xc7 || b1 == 0xc6)
            return 2 + modrm_len(p, 2, modrm_imm_bytes(b1));
        if (b1 == 0xb8 || b1 == 0xb9 || b1 == 0xba || b1 == 0xbb) return 10;  // movabs
        if (b1 == 0x0f) return 5;
        return 0;
    }

    if (b0 == 0x89 || b0 == 0x8b || b0 == 0x85 || b0 == 0x39 || b0 == 0x3b ||
        b0 == 0x2b || b0 == 0x03 || b0 == 0x8d)
        return 1 + modrm_len(p, 1, 0);
    if (b0 == 0x80 || b0 == 0x83 || b0 == 0x81 || b0 == 0xc7 || b0 == 0xc6)
        return 1 + modrm_len(p, 1, modrm_imm_bytes(b0));
    if (b0 == 0xb8 || b0 == 0xb9 || b0 == 0xba || b0 == 0xbb) return 5;
    return 0;
}

// Writes an indirect jump `jmp qword ptr [rip+0]` at `at`, followed by the
// absolute 64-bit target. Works across arbitrary address distances (unlike a
// rel32 jump, which is limited to +-2 GiB).
inline void emit_indirect_jmp(std::uint8_t* at, const void* target) {
    at[0] = 0xFF;
    at[1] = 0x25;  // jmp [rip+disp32]
    const std::uint32_t disp = 0;  // target pointer sits right after (at+6)
    std::memcpy(at + 2, &disp, 4);
    std::memcpy(at + 6, &target, 8);
}

struct Hook {
    void* fn = nullptr;
    void* trampoline = nullptr;
    int stolen_len = 0;
    bool has_endbr = false;
    std::uint8_t orig[24] = {};  // original bytes (stolen region, incl. endbr64)
    std::uint8_t patch[14] = {}; // the 14-byte jmp we wrote
};

// Installs a jump at fn+4 -> detour, with a trampoline that replays the stolen
// bytes and jumps back. Requires fn to start with endbr64.
inline bool install(Hook& out, void* fn, void* detour) {
    auto* base = static_cast<std::uint8_t*>(fn);
    if (!base) return false;
    // Preserve an endbr64 (CET) prefix when present so indirect calls stay
    // IBT-compliant; otherwise patch from the very first byte.
    const bool has_endbr64 =
        base[0] == 0xf3 && base[1] == 0x0f && base[2] == 0x1e && base[3] == 0xfa;
    std::uint8_t* p = base + (has_endbr64 ? 4 : 0);
    const int patch_off = has_endbr64 ? 4 : 0;
    out.has_endbr = has_endbr64;
    for (int i = 0; i < 14; ++i)
        out.orig[patch_off + i] = p[i];
    int len = 0;
    while (len < 16) {  // steal >= 16 bytes (patch writes 14) at instruction boundaries
        const int l = insn_len(p + len);
        if (l <= 0) {
            std::fprintf(stderr, "hook64: cannot decode instruction at +%d (0x%02x)\n",
                         patch_off + len, p[len]);
            return false;
        }
        len += l;
    }
    if (len < 16) {
        std::fprintf(stderr, "hook64: stub too short to patch\n");
        return false;
    }

    // Allocate the trampoline inside a free gap within +-2 GiB of the target
    // function so relocated RIP-relative displacements stay valid.
    const std::size_t tsize = static_cast<std::size_t>(len) + 16;
    const std::uintptr_t fn_addr = reinterpret_cast<std::uintptr_t>(fn);
    void* tramp = MAP_FAILED;
    const std::uintptr_t lo = fn_addr > 0x7FFFFFFFULL ? fn_addr - 0x7FFFFFFFULL : 0;
    const std::uintptr_t hi = fn_addr + 0x7FFFFFFFULL;
    {
        std::FILE* f = std::fopen("/proc/self/maps", "r");
        char line[256];
        std::uintptr_t prev_end = 0;
        std::uintptr_t best_at = 0;
        std::uintptr_t best_dist = ~std::uintptr_t{0};
        while (f && std::fgets(line, sizeof(line), f)) {
            std::uintptr_t start = 0, end = 0;
            if (std::sscanf(line, "%lx-%lx", &start, &end) != 2) continue;
            const std::uintptr_t at = prev_end;
            const std::uintptr_t gap = start - at;
            if (at < start && gap >= tsize && at >= lo && at + tsize <= hi) {
                const std::uintptr_t dist =
                    fn_addr > at ? fn_addr - at : at - fn_addr;
                if (dist < best_dist) {
                    best_dist = dist;
                    best_at = at;
                }
            }
            if (end > prev_end) prev_end = end;
        }
        if (f) std::fclose(f);
        if (best_at) {
            tramp = mmap(reinterpret_cast<void*>(best_at), tsize,
                         PROT_READ | PROT_WRITE | PROT_EXEC,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        }
    }
    if (tramp == MAP_FAILED)
        tramp = mmap(nullptr, tsize, PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (tramp == MAP_FAILED) return false;
    std::memcpy(tramp, p, static_cast<std::size_t>(len));
    // RIP-relative instructions ([rip+disp32]) must be relocated: the
    // trampoline runs at a different address, so rewrite the displacement to
    // keep pointing at the same absolute target.
    {
        const std::uint8_t* src = p;
        std::uint8_t* dst = static_cast<std::uint8_t*>(tramp);
        int off = 0;
        while (off < len) {
            const int ilen = insn_len(src + off);
            if (ilen <= 0) break;
            const std::uint8_t b0 = src[off];
            const bool rex = (b0 & 0xf0) == 0x40 || b0 == 0x41;
            const std::uint8_t opcode = rex ? src[off + 1] : b0;
            const bool modrm_op =
                opcode == 0x89 || opcode == 0x8b || opcode == 0x85 || opcode == 0x39 ||
                opcode == 0x3b || opcode == 0x2b || opcode == 0x03 || opcode == 0x8d ||
                opcode == 0x80 || opcode == 0x81 || opcode == 0x83 || opcode == 0xc6 ||
                opcode == 0xc7;
            if (modrm_op && off + (rex ? 2 : 1) + 1 < len) {
                const int modrm_off = off + (rex ? 2 : 1);
                const std::uint8_t modrm = src[modrm_off];
                if ((modrm & 0xC7) == 0x05) {  // mod==00, rm==101 -> [rip+disp32]
                    const int disp_off = modrm_off + 1;
                    std::int32_t disp = 0;
                    std::memcpy(&disp, src + disp_off, 4);
                    const std::uint8_t* orig_inst = base + patch_off + off;
                    const std::int64_t target =
                        reinterpret_cast<std::intptr_t>(orig_inst) + ilen + disp;
                    const std::int32_t new_disp = static_cast<std::int32_t>(
                        target - (reinterpret_cast<std::intptr_t>(dst + off) + ilen));
                    std::memcpy(dst + disp_off, &new_disp, 4);
                }
            }
            off += ilen;
        }
    }
    // trampoline: replay stolen bytes, then jump back into the original.
    emit_indirect_jmp(static_cast<std::uint8_t*>(tramp) + len, base + patch_off + len);

    // Make the target page writable and patch the entry with `jmp detour`.
    const std::uintptr_t page = reinterpret_cast<std::uintptr_t>(base) & ~0xFFFULL;
    if (mprotect(reinterpret_cast<void*>(page), 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        munmap(tramp, static_cast<std::size_t>(len) + 16);
        return false;
    }
    emit_indirect_jmp(p, detour);
    __builtin___clear_cache(reinterpret_cast<char*>(p),
                            reinterpret_cast<char*>(p) + 14);
    mprotect(reinterpret_cast<void*>(page), 0x1000, PROT_READ | PROT_EXEC);

    out.fn = fn;
    out.trampoline = tramp;
    out.stolen_len = len;
    std::fprintf(stderr, "hook64: hooked %p (stolen %d bytes)\n", fn, len);
    return true;
}

// Restores the original bytes and frees the trampoline. Safe to call even if
// the hook was never installed (fn == nullptr).
inline void uninstall(Hook& h) {
    if (!h.fn) return;
    auto* base = static_cast<std::uint8_t*>(h.fn);
    const int patch_off = h.has_endbr ? 4 : 0;
    const std::uintptr_t page = reinterpret_cast<std::uintptr_t>(base) & ~0xFFFULL;
    if (mprotect(reinterpret_cast<void*>(page), 0x1000,
                 PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
        std::memcpy(base + patch_off, h.orig + patch_off, 14);
        __builtin___clear_cache(reinterpret_cast<char*>(base + patch_off),
                                reinterpret_cast<char*>(base + patch_off) + 14);
        mprotect(reinterpret_cast<void*>(page), 0x1000, PROT_READ | PROT_EXEC);
    }
    if (h.trampoline)
        munmap(h.trampoline, static_cast<std::size_t>(h.stolen_len) + 16);
    h.fn = nullptr;
    h.trampoline = nullptr;
    h.stolen_len = 0;
}

}  // namespace hook64
