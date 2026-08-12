#include "cfg.h"

#include <nlohmann/json.hpp>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {
void cfg_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void cfg_log(const char* fmt, ...) {
    FILE* f = std::fopen("/tmp/cs2_internal.log", "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(f, fmt, ap);
    va_end(ap);
    std::fclose(f);
}

std::uintptr_t parse_hex(const std::string& s) {
    return static_cast<std::uintptr_t>(std::strtoull(s.c_str(), nullptr, 0));
}
}  // namespace

static Config& cfg_impl() {
    cfg_log("cfg: entered\n");
    static Config c;
    static bool init = false;
    if (init) return c;
    init = true;

    const char* path = std::getenv("CS2_CONFIG");
    const char* def = "/home/gugugu/Repo/cs2-cheat/config/cs2_config.json";
    if (!path || !*path) path = def;

    cfg_log("cfg: opening %s\n", path);
    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, "cfg: cannot open %s (using defaults)\n", path);
        cfg_log("cfg: cannot open %s (using defaults)\n", path);
        return c;
    }
    const std::string text((std::istreambuf_iterator<char>(f)),
                           std::istreambuf_iterator<char>());
    cfg_log("cfg: read %zu bytes\n", text.size());

    try {
        // allow_exceptions=false: a malformed file yields a discarded value
        // instead of throwing out of injected code.
        const nlohmann::json root = nlohmann::json::parse(text, nullptr, false);
        cfg_log("cfg: root parsed\n");
        if (root.is_discarded() || !root.is_object()) {
            std::fprintf(stderr, "cfg: bad JSON in %s (using defaults)\n", path);
            cfg_log("cfg: bad JSON in %s (using defaults)\n", path);
            return c;
        }

        cfg_log("cfg: looking for patterns\n");
        if (root.contains("patterns") && root["patterns"].is_array()) {
            for (const auto& e : root["patterns"]) {
                if (!e.is_object()) continue;
                PatternCfg pc;
                pc.name    = e.value("name", std::string());
                pc.pattern = e.value("pattern", std::string());
                pc.off     = e.value("off", 0);
                pc.op      = e.value("op", std::string());
                pc.size    = e.value("size", 4);
                if (!pc.name.empty() && !pc.pattern.empty())
                    c.patterns.push_back(std::move(pc));
            }
        }
        if (root.contains("required") && root["required"].is_array())
            for (const auto& e : root["required"])
                if (e.is_string()) c.required.push_back(e.get<std::string>());

        cfg_log("cfg: patterns done, looking for offsets\n");
        if (root.contains("offsets") && root["offsets"].is_object()) {
            const auto& offs = root["offsets"];
            auto set = [&](const char* k, std::uintptr_t& dst) {
                if (offs.contains(k) && offs[k].is_string())
                    dst = parse_hex(offs[k].get<std::string>());
            };
            set("dwCSGOInput", c.offsets.dwCSGOInput);
            set("viewAngleOffset", c.offsets.viewAngleOffset);
            set("m_vecViewOffset", c.offsets.m_vecViewOffset);
            set("m_ArmorValue", c.offsets.m_ArmorValue);
            set("m_iIDEntIndex", c.offsets.m_iIDEntIndex);
            set("m_pAimPunchServices", c.offsets.m_pAimPunchServices);
            set("aimPunchCache", c.offsets.aimPunchCache);
            set("m_iShotsFired", c.offsets.m_iShotsFired);
            set("m_pGameSceneNode", c.offsets.m_pGameSceneNode);
            set("m_modelState", c.offsets.m_modelState);
            set("boneStateData", c.offsets.boneStateData);
            set("m_hModel", c.offsets.m_hModel);
            set("boneCount", c.offsets.boneCount);
            set("boneNames", c.offsets.boneNames);
            set("boneParents", c.offsets.boneParents);
            set("boneFlags", c.offsets.boneFlags);
            set("boneElementSize", c.offsets.boneElementSize);
            if (offs.contains("boneHeadIndex") && offs["boneHeadIndex"].is_number())
                c.offsets.boneHeadIndex = offs["boneHeadIndex"].get<int>();
            if (offs.contains("boneNeckIndex") && offs["boneNeckIndex"].is_number())
                c.offsets.boneNeckIndex = offs["boneNeckIndex"].get<int>();
            set("m_vecVelocity", c.offsets.m_vecVelocity);
            set("m_flFlashOverlayAlpha", c.offsets.m_flFlashOverlayAlpha);
            set("dwLocalPlayerPawn", c.offsets.dwLocalPlayerPawn);
            if (offs.contains("sensitivity") && offs["sensitivity"].is_number())
                c.offsets.sensitivity = offs["sensitivity"].get<float>();
        }

        c.loaded = true;
        std::fprintf(stderr, "cfg: loaded %zu patterns, %zu offsets from %s\n",
                     c.patterns.size(), c.required.size(), path);
        cfg_log("cfg: loaded %zu patterns, %zu required, sens %.1f, csgoInput=0x%llx\n",
                c.patterns.size(), c.required.size(),
                static_cast<double>(c.offsets.sensitivity),
                static_cast<unsigned long long>(c.offsets.dwCSGOInput));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "cfg: exception %s (using defaults)\n", e.what());
        cfg_log("cfg: exception %s (using defaults)\n", e.what());
        c = Config{};
    }
    return c;
}

const Config& cs2cfg() { return cfg_impl(); }
