#include "patterns.h"

#include "memory.h"
#include "process.h"

#include <sys/uio.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr std::uint32_t kWildcard = 0x100;  // sentinel for '?' in a pattern
constexpr std::size_t kReadChunk = 1u << 20;  // 1 MiB per process_vm_readv

struct Range {
    std::uintptr_t start = 0;
    std::size_t size = 0;
};

// Executable (r-xp) segments of libclient.so, from /proc/<pid>/maps.
std::vector<Range> executable_ranges(int pid) {
    std::vector<Range> out;
    std::ifstream maps("/proc/" + std::to_string(pid) + "/maps");
    std::string line;
    while (std::getline(maps, line)) {
        if (line.find("libclient.so") == std::string::npos) continue;
        const auto dash = line.find('-');
        if (dash == std::string::npos) continue;
        const auto perms_start = line.find(' ', dash);
        if (perms_start == std::string::npos || perms_start + 5 > line.size()) continue;
        if (line[perms_start + 1] != 'r' || line[perms_start + 3] != 'x') continue;
        const auto sp = line.find(' ', perms_start + 1);
        if (sp == std::string::npos) continue;
        const std::uintptr_t start = std::stoull(line.substr(0, dash), nullptr, 16);
        const std::uintptr_t end = std::stoull(line.substr(dash + 1, sp - dash - 1), nullptr, 16);
        if (end > start) out.push_back({start, static_cast<std::size_t>(end - start)});
    }
    return out;
}

// Reads the whole executable section into `text`.
bool read_text(const Memory& mem, const Range& range, std::vector<std::uint8_t>& text) {
    text.resize(range.size);
    std::size_t done = 0;
    while (done < range.size) {
        const std::size_t n = std::min(kReadChunk, range.size - done);
        const iovec local{ text.data() + done, n };
        const iovec remote{ reinterpret_cast<void*>(range.start + done), n };
        const auto r = process_vm_readv(mem.pid(), &local, 1, &remote, 1, 0);
        if (r != static_cast<ssize_t>(n)) return false;
        done += n;
    }
    return true;
}

// "C7 87 ? ? ? ? 00 00 ..." -> vector of bytes; '?' becomes kWildcard.
std::vector<std::uint32_t> parse_pattern(const char* hex) {
    std::vector<std::uint32_t> out;
    std::istringstream ss(hex);
    std::string tok;
    while (ss >> tok) {
        if (tok == "?") {
            out.push_back(kWildcard);
        } else {
            out.push_back(static_cast<std::uint32_t>(std::stoul(tok, nullptr, 16)));
        }
    }
    return out;
}

bool matches(const std::vector<std::uint8_t>& text, std::size_t pos,
             const std::vector<std::uint32_t>& pat) {
    if (pos + pat.size() > text.size()) return false;
    for (std::size_t i = 0; i < pat.size(); ++i) {
        if (pat[i] != kWildcard && text[pos + i] != pat[i]) return false;
    }
    return true;
}

// Returns the absolute address of every match (usually exactly one).
std::vector<std::uintptr_t> find_all(const std::vector<std::uint8_t>& text,
                                     std::uintptr_t base,
                                     const std::vector<std::uint32_t>& pat) {
    std::vector<std::uintptr_t> hits;
    if (pat.empty() || pat.size() > text.size()) return hits;
    for (std::size_t pos = 0; pos + pat.size() <= text.size(); ++pos) {
        if (matches(text, pos, pat)) hits.push_back(base + pos);
    }
    return hits;
}

template <typename T>
std::optional<T> read_at(const Memory& mem, std::uintptr_t addr) {
    T v{};
    if (!mem.read(addr, &v, sizeof(T))) return std::nullopt;
    return v;
}

struct ScanResult {
    bool found = false;
    std::uintptr_t match = 0;   // absolute address of the pattern match
    int occurrences = 0;
};

}  // namespace

namespace patterns {

namespace {

constexpr Pattern kPatterns[] = {
    // ---- entity system -----------------------------------------------------
    // EntitySystemPointer (mov [rip+disp32], rbx) -> CGameEntitySystem** slot
    {"4C 63 ? ? ? ? ? 48 89 1D ? ? ? ?", 10, Op::Abs4},
    // EntityListOffset (lea r13, [rdi+disp8]) -> entity system + off = list
    {"4C 8D 6F ? 41 54 53 48 89 FB 48 83 EC ? 48 89 07 48", 3, Op::Read, 1},
    // ---- C_BaseEntity ------------------------------------------------------
    // m_pGameSceneNode (mov rax, [rbp+disp32])
    {"2C E0 49 8B 85 ? ? ? ?", 5, Op::Read},
    // m_iHealth (mov dword ptr [rdi+disp32], 0)
    {"C7 87 ? ? ? ? 00 00 00 00 48 8D 35", 2, Op::Read},
    // m_lifeState (movzx edx, byte ptr [rdi+disp32])
    {"0F B6 97 ? ? ? ? 39 F2", 3, Op::Read},
    // m_iTeamNum (cmp byte ptr [rdi+disp32], 2)
    {"? ? ? ? 02 48 8D 05 ? ? ? ? 74 ? 48", 0, Op::Read},
    // ---- CCSPlayerController ------------------------------------------------
    // m_hPawn (mov edi, [rdi+disp32])
    {"84 C0 75 ? 8B 8F ? ? ? ?", 6, Op::Read},
    // ---- C_CSPlayerPawn -----------------------------------------------------
    // m_pWeaponServices (mov rdi, [rsi+disp32])
    {"48 8B BE ? ? ? ? 48 8D 35 ? ? ? ? E8 ? ? ? ? 48 89 C2", 3, Op::Read},
    // m_bIsScoped (mov ebx, disp32)
    {"BB ? ? ? ? 00 F3 0F 11 45 ? 0F", 1, Op::Read},
    // ---- client globals -----------------------------------------------------
    // dwLocalPlayerController (cmp qword ptr [rip+disp32], 0)
    {"48 83 3D ? ? ? ? ? 0F 95 C0 C3", 3, Op::Abs5},
    // dwGlobalVars (mov [rip+disp32], rsi)
    {"8D ? ? ? ? ? 48 89 35 ? ? ? ? 48 89 ? ? C3", 9, Op::Abs4},
    // dwViewMatrix (lea r8, [rip+disp32])
    {"01 4C 8D 05 ? ? ? ? 4C 89 EE", 4, Op::Abs4},
    // dwViewRender (lea rax, [rip+disp32])
    {"48 8D 05 ? ? ? ? 48 89 38 48 85", 3, Op::Abs4},
};

constexpr int kRequired[] = {
    0,  // gameEntitySystem
    1,  // entityListOffset
    2,  // m_pGameSceneNode
    3,  // m_iHealth
    4,  // m_lifeState
    5,  // m_iTeamNum
    6,  // m_hPawn
    9,  // localPlayerController
    11, // viewMatrix
};

struct Slot {
    Pattern pat;
    std::uintptr_t value = 0;  // abs address (Abs) or offset (Read)
    bool is_offset = false;
    ScanResult scan;
};

}  // namespace

bool resolve(int pid, Resolved& out) {
    const auto ranges = executable_ranges(pid);
    if (ranges.empty()) {
        std::fprintf(stderr, "patterns: no executable libclient.so segment found\n");
        return false;
    }

    Memory mem;
    mem.attach(pid);
    std::vector<std::uint8_t> text;
    if (!read_text(mem, ranges.front(), text)) {
        std::fprintf(stderr, "patterns: failed to read libclient.so .text (%zu MiB)\n",
                     ranges.front().size >> 20);
        return false;
    }
    std::fprintf(stderr, "patterns: scanned %zu MiB of libclient.so code\n",
                 ranges.front().size >> 20);

    Slot slots[sizeof(kPatterns) / sizeof(kPatterns[0])];
    for (std::size_t i = 0; i < sizeof(kPatterns) / sizeof(kPatterns[0]); ++i) {
        slots[i].pat = kPatterns[i];
        const auto bytes = parse_pattern(kPatterns[i].hex);
        const auto hits = find_all(text, ranges.front().start, bytes);
        slots[i].scan.found = !hits.empty();
        slots[i].scan.occurrences = static_cast<int>(hits.size());
        slots[i].scan.match = hits.empty() ? 0 : hits.front();

        if (slots[i].scan.found) {
            const std::uintptr_t match = slots[i].scan.match;
            switch (kPatterns[i].op) {
            case Op::Read: {
                if (kPatterns[i].read_size == 1) {
                    if (const auto v = read_at<std::int8_t>(mem, match + kPatterns[i].add))
                        slots[i].value = static_cast<std::uintptr_t>(*v);
                } else {
                    if (const auto v = read_at<std::int32_t>(mem, match + kPatterns[i].add))
                        slots[i].value = static_cast<std::uintptr_t>(*v);
                }
                slots[i].is_offset = true;
                break;
            }
            case Op::Abs4:
            case Op::Abs5: {
                const auto disp = read_at<std::int32_t>(mem, match + kPatterns[i].add);
                if (disp)
                    slots[i].value = match + kPatterns[i].add +
                                     (kPatterns[i].op == Op::Abs4 ? 4 : 5) +
                                     static_cast<std::uintptr_t>(*disp);
                break;
            }
            case Op::None:
                slots[i].value = match + kPatterns[i].add;
                break;
            }
        }
    }

    out.gameEntitySystem = slots[0].value;
    out.entityListOffset = static_cast<int>(slots[1].value);
    out.m_pGameSceneNode = static_cast<int>(slots[2].value);
    out.m_iHealth = static_cast<int>(slots[3].value);
    out.m_lifeState = static_cast<int>(slots[4].value);
    out.m_iTeamNum = static_cast<int>(slots[5].value);
    out.m_hPawn = static_cast<int>(slots[6].value);
    out.m_pWeaponServices = static_cast<int>(slots[7].value);
    out.m_bIsScoped = static_cast<int>(slots[8].value);
    out.localPlayerController = slots[9].value;
    out.globalVars = slots[10].value;
    out.viewMatrix = slots[11].value;
    out.viewRender = slots[12].value;

    out.ok = true;
    for (const int idx : kRequired) {
        if (!slots[idx].scan.found) {
            std::fprintf(stderr, "patterns: MISSING pattern #%d: %s\n", idx, kPatterns[idx].hex);
            out.ok = false;
        } else if (slots[idx].scan.occurrences > 1) {
            std::fprintf(stderr, "patterns: pattern #%d matched %d times: %s\n", idx,
                         slots[idx].scan.occurrences, kPatterns[idx].hex);
        }
    }

    std::fprintf(stderr,
                 "patterns: resolved -> gs=0x%llx listOff=%d scene=%d health=%d life=%d team=%d "
                 "hPawn=%d ctrl=0x%llx viewMatrix=0x%llx weaponServices=%d scoped=%d\n",
                 static_cast<unsigned long long>(out.gameEntitySystem), out.entityListOffset,
                 out.m_pGameSceneNode, out.m_iHealth, out.m_lifeState, out.m_iTeamNum, out.m_hPawn,
                 static_cast<unsigned long long>(out.localPlayerController),
                 static_cast<unsigned long long>(out.viewMatrix), out.m_pWeaponServices,
                 out.m_bIsScoped);
    return out.ok;
}

}  // namespace patterns
