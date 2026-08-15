#include "patterns.h"

#include "cfg.h"
#include "logger.h"
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

bool OffsetResolver::attach(int pid) {
    Logger::instance().log("patterns: resolve start\n");
    pid_ = pid;
    const auto ranges = executable_ranges(pid);
    if (ranges.empty()) {
        Logger::instance().error("patterns: FAILED no executable libclient.so segment\n");
        return false;
    }

    Memory mem;
    mem.attach(pid);
    std::vector<std::uint8_t> text;
    if (!read_text(mem, ranges.front(), text)) {
        Logger::instance().error("patterns: FAILED read libclient.so .text (%zu MiB)\n",
                                 ranges.front().size >> 20);
        return false;
    }
    Logger::instance().log("patterns: scanned %zu MiB of libclient.so code\n",
                           ranges.front().size >> 20);
    Logger::instance().log("patterns: scanned %zu MiB @0x%llx\n", ranges.front().size >> 20,
                           static_cast<unsigned long long>(ranges.front().start));

    Logger::instance().log("patterns: loading config\n");
    const auto& conf = Config::instance();
    Logger::instance().log("patterns: config loaded, %zu patterns\n", conf.patterns.size());
    std::vector<Slot> slots;
    slots.reserve(conf.patterns.size());
    for (const auto& pc : conf.patterns) {
        Slot s;
        s.pat.hex = pc.pattern.c_str();
        s.pat.add = pc.off;
        s.pat.op = pc.op == "read"      ? Op::Read
                   : pc.op == "abs4"      ? Op::Abs4
                   : pc.op == "abs5"      ? Op::Abs5
                                          : Op::None;
        s.pat.read_size = pc.size;
        s.pat.len = pc.len;

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
                              static_cast<std::uintptr_t>(s.pat.len) +
                              static_cast<std::uintptr_t>(*disp);
                break;
            }

            case Op::None:
                s.value = match + s.pat.add;
                break;
            }
        }
        Logger::instance().log("patterns: %-22s found=%d occ=%d val=0x%llx\n", pc.name.c_str(),
                               s.scan.found ? 1 : 0, s.scan.occurrences,
                               static_cast<unsigned long long>(s.value));
        slots.push_back(std::move(s));
    }

    auto get = [&](const char* name) -> std::uintptr_t {
        for (std::size_t i = 0; i < conf.patterns.size(); ++i)
            if (conf.patterns[i].name == name) return slots[i].value;
        return 0;
    };
    out_.gameEntitySystem = get("gameEntitySystem");
    out_.entityListOffset = static_cast<int>(get("entityListOffset"));
    out_.m_pGameSceneNode = static_cast<int>(get("m_pGameSceneNode"));
    out_.m_iHealth = static_cast<int>(get("m_iHealth"));
    out_.m_lifeState = static_cast<int>(get("m_lifeState"));
    out_.m_iTeamNum = static_cast<int>(get("m_iTeamNum"));
    out_.m_hPawn = static_cast<int>(get("m_hPawn"));
    out_.m_pWeaponServices = static_cast<int>(get("m_pWeaponServices"));
    out_.m_bIsScoped = static_cast<int>(get("m_bIsScoped"));
    out_.localPlayerController = get("localPlayerController");
    out_.globalVars = get("globalVars");
    out_.viewMatrix = get("viewMatrix");
    out_.viewRender = get("viewRender");
    out_.vphysWorld = get("vphysWorld");

    out_.ok = true;
    for (const auto& name : conf.required) {
        const std::uintptr_t v = get(name.c_str());
        if (v == 0) {
            Logger::instance().error("patterns: MISSING pattern: %s\n", name.c_str());
            out_.ok = false;
        }
    }

    Logger::instance().log(
                 "patterns: resolved -> gs=0x%llx listOff=%d scene=%d health=%d life=%d team=%d "
                 "hPawn=%d ctrl=0x%llx viewMatrix=0x%llx weaponServices=%d scoped=%d\n",
                 static_cast<unsigned long long>(out_.gameEntitySystem), out_.entityListOffset,
                 out_.m_pGameSceneNode, out_.m_iHealth, out_.m_lifeState, out_.m_iTeamNum, out_.m_hPawn,
                 static_cast<unsigned long long>(out_.localPlayerController),
                 static_cast<unsigned long long>(out_.viewMatrix), out_.m_pWeaponServices,
                 out_.m_bIsScoped);
    Logger::instance().log("patterns: resolved ok=%d -> gs=0x%llx listOff=%d scene=%d health=%d life=%d team=%d "
                           "hPawn=%d ctrl=0x%llx viewMatrix=0x%llx\n",
                           out_.ok ? 1 : 0,
                           static_cast<unsigned long long>(out_.gameEntitySystem), out_.entityListOffset,
                           out_.m_pGameSceneNode, out_.m_iHealth, out_.m_lifeState, out_.m_iTeamNum, out_.m_hPawn,
                           static_cast<unsigned long long>(out_.localPlayerController),
                           static_cast<unsigned long long>(out_.viewMatrix));
    return out_.ok;
}

}  // namespace patterns
