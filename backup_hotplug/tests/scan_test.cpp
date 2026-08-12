#include <sys/uio.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fstream>
#include <string>
#include <vector>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>

// Copy of the scan logic from internal/vk_hook.cpp, exercised standalone.
static int patch_memory_value(std::uintptr_t needle, std::uintptr_t replacement) {
    int replaced = 0;
    std::ifstream maps("/proc/self/maps");
    std::string line;
    while (std::getline(maps, line)) {
        const auto dash = line.find('-');
        if (dash == std::string::npos) continue;
        const auto sp = line.find(' ', dash);
        if (sp == std::string::npos) continue;
        if (sp + 3 >= line.size() || line[sp + 1] != 'r') continue;
        if (line[sp + 3] == 'x') continue;
        const bool writable = line[sp + 2] == 'w';
        const std::uintptr_t start = std::stoull(line.substr(0, dash), nullptr, 16);
        const std::uintptr_t end = std::stoull(line.substr(dash + 1, sp - dash - 1), nullptr, 16);
        const std::uintptr_t size = end - start;
        if (size == 0 || size > (1ull << 30)) continue;
        if (line.find("/dev/") != std::string::npos) continue;
        std::uintptr_t addr = start;
        std::vector<std::uint8_t> buf(4u << 20);
        while (addr < end) {
            const std::size_t want =
                static_cast<std::size_t>(std::min<std::uintptr_t>(buf.size(), end - addr));
            const iovec local{ buf.data(), want };
            const iovec remote{ reinterpret_cast<void*>(addr), want };
            const auto r = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
            if (r != static_cast<ssize_t>(want)) { addr += want; continue; }
            for (std::size_t i = 0; i + 8 <= want; ++i) {
                std::uint64_t v = 0;
                std::memcpy(&v, buf.data() + i, 8);
                if (v == needle) {
                    std::uintptr_t target = addr + i;
                    if (!writable) {
                        mprotect(reinterpret_cast<void*>(target & ~0xFFFULL), 0x1000,
                                 PROT_READ | PROT_WRITE);
                    }
                    std::memcpy(reinterpret_cast<void*>(target), &replacement, 8);
                    if (!writable) {
                        mprotect(reinterpret_cast<void*>(target & ~0xFFFULL), 0x1000, PROT_READ);
                    }
                    if (++replaced >= 64) return replaced;
                }
            }
            addr += want;
        }
    }
    return replaced;
}

// --- targets --------------------------------------------------------------
static const std::uintptr_t kNeedle = 0x12345678ABCDEF00ull;
static const std::uintptr_t kRepl = 0xDEADBEEFCAFEF00Dull;

// .bss (writable)
static std::uintptr_t g_bss_slot = 0;
// .data (writable, initialized)
static std::uintptr_t g_data_slot = kNeedle;
// .rodata (read-only)
static const std::uintptr_t g_ro_slot = kNeedle;

int main() {
    g_bss_slot = kNeedle;
    std::uintptr_t* heap = new std::uintptr_t(kNeedle);
    std::printf("slots: bss=%p data=%p ro=%p heap=%p\n",
                (void*)&g_bss_slot, (void*)&g_data_slot, (void*)&g_ro_slot, (void*)heap);

    const int n = patch_memory_value(kNeedle, kRepl);
    std::printf("patched %d slots\n", n);

    int ok = 0;
    if (g_bss_slot == kRepl) { std::printf("bss    OK\n"); ok++; }
    else std::printf("bss    FAIL (0x%llx)\n", (unsigned long long)g_bss_slot);
    if (g_data_slot == kRepl) { std::printf("data   OK\n"); ok++; }
    else std::printf("data   FAIL (0x%llx)\n", (unsigned long long)g_data_slot);
    // Read through a volatile pointer: the compiler folds plain reads of a
    // `const` object back to the initializer even after we patched memory.
    const volatile std::uintptr_t* ro = &g_ro_slot;
    if (*ro == kRepl) { std::printf("rodata OK\n"); ok++; }
    else std::printf("rodata FAIL (0x%llx)\n", (unsigned long long)*ro);
    if (*heap == kRepl) { std::printf("heap   OK\n"); ok++; }
    else std::printf("heap   FAIL (0x%llx)\n", (unsigned long long)*heap);

    std::printf("RESULT: %d/4\n", ok);
    return ok == 4 ? 0 : 1;
}
