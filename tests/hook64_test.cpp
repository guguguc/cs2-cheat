#include "../internal/hook_x64.h"

#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

int main(int argc, char** argv) {
    const char* so =
        "/home/gugugu/.local/share/Steam/steamapps/common/Counter-Strike "
        "Global Offensive/game/csgo/bin/linuxsteamrt64/libclient.so";
    FILE* f = std::fopen(so, "rb");
    if (!f) {
        std::printf("cannot open %s\n", so);
        return 1;
    }
    // Function RVA 0x1854970 -> file offset = RVA - 0x1000 for the rx segment.
    std::fseek(f, 0x1854970 - 0x1000, SEEK_SET);
    unsigned char prologue[40] = {0};
    const std::size_t got = std::fread(prologue, 1, sizeof prologue, f);
    std::fclose(f);
    std::printf("prologue bytes (%zu):", got);
    for (std::size_t i = 0; i < got; ++i) std::printf(" %02x", prologue[i]);
    std::printf("\n");

    // Decode check
    int off = 0;
    while (off < (int)got) {
        const int l = hook64::insn_len(prologue + off);
        std::printf("  +%02d: len=%d bytes:", off, l);
        if (l <= 0) {
            std::printf(" (unknown instr)\n");
            return 1;
        }
        for (int i = 0; i < l; ++i) std::printf(" %02x", prologue[off + i]);
        std::printf("\n");
        off += l;
    }

    // Copy to an executable page and run hook64::install
    void* mem = mmap(nullptr, 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        std::printf("mmap failed\n");
        return 1;
    }
    std::memcpy(mem, prologue, sizeof prologue);

    hook64::Hook h;
    bool ok = hook64::install(h, mem, (void*)((char*)mem + 0x1000));
    std::printf("hook64::install -> %s (trampoline=%p stolen=%d)\n",
                ok ? "OK" : "FAILED", h.trampoline, h.stolen_len);
    return ok ? 0 : 2;
}
