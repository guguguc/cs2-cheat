#include "cfg.h"
#include "logger.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {
std::uintptr_t parse_hex(const std::string& s) {
    return static_cast<std::uintptr_t>(std::strtoull(s.c_str(), nullptr, 0));
}
}  // namespace

Config& Config::instance() {
    static Config c;
    return c;
}

Config::Config() {
    load();
}

void Config::load() {
    Logger::instance().log("cfg: loaded once\n");

    const char* path = std::getenv("CS2_CONFIG");
    const char* def = "/home/gugugu/Repo/cs2-cheat/config/cs2_config.json";
    if (!path || !*path) path = def;

    Logger::instance().log("cfg: opening %s\n", path);
    std::ifstream f(path);
    if (!f) {
        Logger::instance().error("cfg: cannot open %s (using defaults)\n", path);
        return;
    }
    const std::string text((std::istreambuf_iterator<char>(f)),
                           std::istreambuf_iterator<char>());
    Logger::instance().log("cfg: read %zu bytes\n", text.size());

    try {
        // allow_exceptions=false: a malformed file yields a discarded value
        // instead of throwing out of injected code.
        const nlohmann::json root = nlohmann::json::parse(text, nullptr, false);
        Logger::instance().log("cfg: root parsed\n");
        if (root.is_discarded() || !root.is_object()) {
            Logger::instance().error("cfg: bad JSON in %s (using defaults)\n", path);
            return;
        }

        Logger::instance().log("cfg: looking for patterns\n");
        if (root.contains("patterns") && root["patterns"].is_array()) {
            for (const auto& e : root["patterns"]) {
                if (!e.is_object()) continue;
                PatternCfg pc;
                pc.name    = e.value("name", std::string());
                pc.pattern = e.value("pattern", std::string());
                pc.off     = e.value("off", 0);
                pc.op      = e.value("op", std::string());
                pc.size    = e.value("size", 4);
                pc.len     = e.value("len", 4);
                if (!pc.name.empty() && !pc.pattern.empty())
                    patterns.push_back(std::move(pc));
            }
        }
        if (root.contains("required") && root["required"].is_array())
            for (const auto& e : root["required"])
                if (e.is_string()) required.push_back(e.get<std::string>());

        Logger::instance().log("cfg: patterns done, looking for offsets\n");
        if (root.contains("offsets") && root["offsets"].is_object()) {
            const auto& offs = root["offsets"];
            auto set = [&](const char* k, std::uintptr_t& dst) {
                if (offs.contains(k) && offs[k].is_string())
                    dst = parse_hex(offs[k].get<std::string>());
            };
            set("dwCSGOInput", offsets.dwCSGOInput);
            set("viewAngleOffset", offsets.viewAngleOffset);
            set("m_vecViewOffset", offsets.m_vecViewOffset);
            set("m_ArmorValue", offsets.m_ArmorValue);
            set("m_iIDEntIndex", offsets.m_iIDEntIndex);
            set("m_pAimPunchServices", offsets.m_pAimPunchServices);
            set("aimPunchCache", offsets.aimPunchCache);
            set("m_iShotsFired", offsets.m_iShotsFired);
            set("m_pWeaponServices", offsets.m_pWeaponServices);
            set("m_flFOVSensitivityAdjust", offsets.m_flFOVSensitivityAdjust);
            set("m_hActiveWeapon", offsets.m_hActiveWeapon);
            set("m_designerName", offsets.m_designerName);
            set("m_pGameSceneNode", offsets.m_pGameSceneNode);
            set("m_modelState", offsets.m_modelState);
            set("boneStateData", offsets.boneStateData);
            set("m_hModel", offsets.m_hModel);
            set("boneCount", offsets.boneCount);
            set("boneNames", offsets.boneNames);
            set("boneParents", offsets.boneParents);
            set("boneFlags", offsets.boneFlags);
            set("boneElementSize", offsets.boneElementSize);
            if (offs.contains("boneHeadIndex") && offs["boneHeadIndex"].is_number())
                offsets.boneHeadIndex = offs["boneHeadIndex"].get<int>();
            if (offs.contains("boneNeckIndex") && offs["boneNeckIndex"].is_number())
                offsets.boneNeckIndex = offs["boneNeckIndex"].get<int>();
            set("m_vecVelocity", offsets.m_vecVelocity);
            set("m_flFlashOverlayAlpha", offsets.m_flFlashOverlayAlpha);
            set("dwLocalPlayerPawn", offsets.dwLocalPlayerPawn);
            if (offs.contains("sensitivity") && offs["sensitivity"].is_number())
                offsets.sensitivity = offs["sensitivity"].get<float>();
        }

        loaded = true;
        Logger::instance().error("cfg: loaded %zu patterns, %zu offsets from %s\n",
                                 patterns.size(), required.size(), path);
        Logger::instance().log("cfg: loaded %zu patterns, %zu required, sens %.1f, csgoInput=0x%llx\n",
                patterns.size(), required.size(),
                static_cast<double>(offsets.sensitivity),
                static_cast<unsigned long long>(offsets.dwCSGOInput));
    } catch (const std::exception& e) {
        Logger::instance().error("cfg: exception %s (using defaults)\n", e.what());
        patterns.clear();
        required.clear();
        offsets = OffsetCfg{};
        loaded = false;
    }
}
