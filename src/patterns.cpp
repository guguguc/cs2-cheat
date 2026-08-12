#include "patterns.h"

#include "cfg.h"
#include "memory.h"
#include "process.h"

#include <sys/uio.h>

#include <algorithm>
#include <cstdarg>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

void plog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void plog(const char* fmt, ...) {
    FILE* f = std::fopen("/tmp/cs2_internal.log", "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(f, fmt, ap);
    va_end(ap);
    std::fclose(f);
}

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
        if (tok == "?" || tok == "??") {
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

struct Slot {
    Pattern pat;
    std::uintptr_t value = 0;  // abs address (Abs) or offset (Read)
    bool is_offset = false;
    ScanResult scan;
};

}  // namespace

bool resolve(int pid, Resolved& out) {
    plog("patterns: resolve start\n");
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

    plog("patterns: loading config\n");
    const auto& conf = cs2cfg();
    plog("patterns: config loaded, %zu patterns\n", conf.patterns.size());
    std::vector<Slot> slots;
    slots.reserve(conf.patterns.size());
    for (const auto& pc : conf.patterns) {
        Slot s;
        s.pat.hex = pc.pattern.c_str();
        s.pat.add = pc.off;
        s.pat.op = pc.op == "read"   ? Op::Read
                   : pc.op == "abs4" ? Op::Abs4
                   : pc.op == "abs5" ? Op::Abs5
                                     : Op::None;
        s.pat.read_size = pc.size;

        const auto bytes = parse_pattern(s.pat.hex);
        const auto hits = find_all(text, ranges.front().start, bytes);
        s.scan.found = !hits.empty();
        s.scan.occurrences = static_cast<int>(hits.size());
        s.scan.match = hits.empty() ? 0 : hits.front();

        if (s.scan.found) {
            const std::uintptr_t match = s.scan.match;
            switch (s.pat.op) {
            case Op::Read: {
                if (s.pat.read_size == 1) {
                    if (const auto v = read_at<std::int8_t>(mem, match + s.pat.add))
                        s.value = static_cast<std::uintptr_t>(*v);
                } else {
                    if (const auto v = read_at<std::int32_t>(mem, match + s.pat.add))
                        s.value = static_cast<std::uintptr_t>(*v);
                }
                s.is_offset = true;
                break;
            }
            case Op::Abs4:
            case Op::Abs5: {
                const auto disp = read_at<std::int32_t>(mem, match + s.pat.add);
                if (disp)
                    s.value = match + s.pat.add +
                              (s.pat.op == Op::Abs4 ? 4 : 5) +
                              static_cast<std::uintptr_t>(*disp);
                break;
            }
            case Op::None:
                s.value = match + s.pat.add;
                break;
            }
        }
        slots.push_back(std::move(s));
    }

    auto get = [&](const char* name) -> std::uintptr_t {
        for (std::size_t i = 0; i < conf.patterns.size(); ++i)
            if (conf.patterns[i].name == name) return slots[i].value;
        return 0;
    };
    out.gameEntitySystem = get("gameEntitySystem");
    out.entityListOffset = static_cast<int>(get("entityListOffset"));
    out.m_pGameSceneNode = static_cast<int>(get("m_pGameSceneNode"));
    out.m_iHealth = static_cast<int>(get("m_iHealth"));
    out.m_lifeState = static_cast<int>(get("m_lifeState"));
    out.m_iTeamNum = static_cast<int>(get("m_iTeamNum"));
    out.m_hPawn = static_cast<int>(get("m_hPawn"));
    out.m_pWeaponServices = static_cast<int>(get("m_pWeaponServices"));
    out.m_bIsScoped = static_cast<int>(get("m_bIsScoped"));
    out.localPlayerController = get("localPlayerController");
    out.globalVars = get("globalVars");
    out.viewMatrix = get("viewMatrix");
    out.viewRender = get("viewRender");

    out.ok = true;
    for (const auto& name : conf.required) {
        const std::uintptr_t v = get(name.c_str());
        if (v == 0) {
            std::fprintf(stderr, "patterns: MISSING pattern: %s\n", name.c_str());
            out.ok = false;
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
