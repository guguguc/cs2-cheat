// Self-contained verification of the hook64 inline-hook machinery.
//
// 1. Instruction decoder: checks insn_len/modrm_len against known byte
//    sequences (no game files needed).
// 2. Install/trampoline/uninstall round-trip on a self-built executable page,
//    proving that the detour fires, the trampoline replays the stolen bytes
//    and uninstall restores the original function.
#include "hook_x64.h"

#include <sys/mman.h>

#include <cstdio>
#include <cstring>

namespace {

int fails = 0;

#define CHECK(cond, ...)                                          \
    do {                                                          \
        if (!(cond)) {                                            \
            std::printf("FAIL: " __VA_ARGS__);                    \
            std::printf("\n");                                    \
            ++fails;                                              \
        }                                                         \
    } while (0)

// ---------------------------------------------------------------------------
// 1. instruction decoder
// ---------------------------------------------------------------------------
struct DecodeCase {
    const char* name;
    std::uint8_t bytes[10];
    int want;
};

int test_decoder() {
    const DecodeCase cases[] = {
        {"endbr64",         {0xF3, 0x0F, 0x1E, 0xFA}, 4},
        {"nop",             {0x90}, 1},
        {"ret",             {0xC3}, 1},
        {"push rbx",        {0x53}, 1},
        {"mov rbp,rsp",     {0x48, 0x89, 0xE5}, 3},
        {"sub rsp,imm8",    {0x48, 0x83, 0xEC, 0x10}, 4},
        {"lea rax,[rip]",   {0x48, 0x8D, 0x05, 0, 0, 0, 0}, 7},
        {"mov rax,imm64",   {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0}, 10},
        {"call rel32",      {0xE8, 0, 0, 0, 0}, 5},
        {"jmp rel8",        {0xEB, 0}, 2},
        {"jcc rel32",       {0x0F, 0x84, 0, 0, 0, 0}, 6},
    };
    for (const auto& c : cases) {
        const int got = hook64::Hook::insn_len(c.bytes);
        CHECK(got == c.want, "insn_len(%s): got %d want %d", c.name, got, c.want);
    }

    // modrm_len: [rip+disp32] (mod=0, rm=5) = 1(modrm) + 4(disp) + imm_bytes
    CHECK(hook64::Hook::modrm_len(
              (const std::uint8_t*)"\x05\x00\x00\x00\x00", 0, 0) == 5,
          "modrm_len [rip+disp32]");
    // register operand (mod=3): just the modrm byte
    CHECK(hook64::Hook::modrm_len((const std::uint8_t*)"\xe5", 0, 0) == 1,
          "modrm_len reg");
    return fails;
}

// ---------------------------------------------------------------------------
// 2. install / trampoline / uninstall round-trip
// ---------------------------------------------------------------------------
// target: endbr64; mov eax, 0xDEADBEEF; nop*15; ret  (25 bytes; the patch
// region after endbr64 has 20 bytes so install can safely steal 16)
const std::uint8_t kTarget[] = {
    0xF3, 0x0F, 0x1E, 0xFA,                              // endbr64
    0xB8, 0xEF, 0xBE, 0xAD, 0xDE,                       // mov eax, 0xDEADBEEF
    0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,           // nop * 7
    0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,     // nop * 8
    0xC3,                                                // ret
};
static_assert(sizeof kTarget == 25, "target must be exactly 25 bytes");
// detour: mov eax, 0x12345678; ret
const std::uint8_t kDetour[] = {
    0xB8, 0x78, 0x56, 0x34, 0x12,
    0xC3,
};

int test_roundtrip() {
    void* target_page = mmap(nullptr, 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    void* detour_page = mmap(nullptr, 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (target_page == MAP_FAILED || detour_page == MAP_FAILED) {
        std::printf("FAIL: mmap\n");
        return ++fails;
    }
    std::memcpy(target_page, kTarget, sizeof kTarget);
    std::memcpy(detour_page, kDetour, sizeof kDetour);

    auto call = [](void* fn) { return reinterpret_cast<int (*)()>(fn)(); };
    const int kTargetRet = static_cast<int>(0xDEADBEEF);
    const int kDetourRet = static_cast<int>(0x12345678);

    // Baseline: original function.
    CHECK(call(target_page) == kTargetRet, "baseline call = 0x%x", call(target_page));

    // Install and verify the detour fires.
    hook64::Hook h;
    CHECK(h.install(target_page, detour_page), "install");
    CHECK(h.installed(), "installed()");
    CHECK(h.target() == target_page, "target()");
    CHECK(call(target_page) == kDetourRet, "detour call = 0x%x", call(target_page));

    // Trampoline replays the stolen bytes (mov eax, 0xDEADBEEF) and jumps back
    // into the original ret.
    CHECK(h.trampoline() != nullptr, "trampoline allocated");
    CHECK(call(h.trampoline()) == kTargetRet, "trampoline call = 0x%x",
          call(h.trampoline()));

    // Uninstall restores the original function.
    h.uninstall();
    CHECK(!h.installed(), "uninstalled()");
    CHECK(call(target_page) == kTargetRet, "post-uninstall call = 0x%x",
          call(target_page));

    munmap(target_page, 0x2000);
    munmap(detour_page, 0x2000);
    return fails;
}

}  // namespace

int main() {
    test_decoder();
    test_roundtrip();

    if (fails == 0)
        std::printf("hook64_test: all passed\n");
    else
        std::printf("hook64_test: %d FAILURES\n", fails);
    return fails == 0 ? 0 : 1;
}
