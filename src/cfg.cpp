#include "cfg.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

// ---- minimal JSON parser (objects/arrays/strings/numbers/bools) ------------
struct Json {
    enum Type { Obj, Arr, Str, Num, Bool, Null } type = Null;
    std::vector<std::pair<std::string, Json>> obj;
    std::vector<Json> arr;
    std::string str;
    double num = 0;
    bool boolean = false;
};

void skip_ws(const char*& p) {
    while (*p && std::isspace(static_cast<unsigned char>(*p))) ++p;
}

bool parse_string(const char*& p, std::string& out) {
    if (*p != '"') return false;
    ++p;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) {
            ++p;
            out += *p == 'n' ? '\n' : *p == 't' ? '\t' : *p == 'r' ? '\r' : *p;
        } else {
            out += *p;
        }
        ++p;
    }
    if (*p != '"') return false;
    ++p;
    return true;
}

bool parse_number(const char*& p, double& out) {
    const char* s = p;
    if (*p == '-' || *p == '+') ++p;
    if (!std::isdigit(static_cast<unsigned char>(*p))) return false;
    while (std::isdigit(static_cast<unsigned char>(*p))) ++p;
    if (*p == '.') {
        ++p;
        while (std::isdigit(static_cast<unsigned char>(*p))) ++p;
    }
    out = std::strtod(s, nullptr);
    return true;
}

Json parse_value(const char*& p) {
    skip_ws(p);
    Json j;
    if (*p == '{') {
        j.type = Json::Obj;
        ++p;
        skip_ws(p);
        if (*p == '}') { ++p; return j; }
        while (true) {
            skip_ws(p);
            std::string key;
            if (!parse_string(p, key)) break;
            skip_ws(p);
            if (*p != ':') break;
            ++p;
            j.obj.emplace_back(std::move(key), parse_value(p));
            skip_ws(p);
            if (*p == ',') { ++p; continue; }
            if (*p == '}') { ++p; break; }
            break;
        }
    } else if (*p == '[') {
        j.type = Json::Arr;
        ++p;
        skip_ws(p);
        if (*p == ']') { ++p; return j; }
        while (true) {
            j.arr.push_back(parse_value(p));
            skip_ws(p);
            if (*p == ',') { ++p; continue; }
            if (*p == ']') { ++p; break; }
            break;
        }
    } else if (*p == '"') {
        j.type = Json::Str;
        parse_string(p, j.str);
    } else if (*p == 't' && std::strncmp(p, "true", 4) == 0) {
        j.type = Json::Bool;
        j.boolean = true;
        p += 4;
    } else if (*p == 'f' && std::strncmp(p, "false", 5) == 0) {
        j.type = Json::Bool;
        j.boolean = false;
        p += 5;
    } else if (*p == 'n' && std::strncmp(p, "null", 4) == 0) {
        j.type = Json::Null;
        p += 4;
    } else if (parse_number(p, j.num)) {
        j.type = Json::Num;
    }
    return j;
}

const Json* find_obj(const Json& j, const char* key) {
    if (j.type != Json::Obj) return nullptr;
    for (const auto& [k, v] : j.obj)
        if (k == key) return &v;
    return nullptr;
}

std::uintptr_t parse_hex(const std::string& s) {
    return static_cast<std::uintptr_t>(std::strtoull(s.c_str(), nullptr, 0));
}

}  // namespace

const Config& cs2cfg() {
    static Config c;
    static bool init = false;
    if (init) return c;
    init = true;

    const char* path = std::getenv("CS2_CONFIG");
    const char* def = "/home/gugugu/Repo/cs2-cheat/config/cs2_config.json";
    if (!path || !*path) path = def;

    std::FILE* f = std::fopen(path, "rb");
    if (!f) {
        std::fprintf(stderr, "cfg: cannot open %s (using defaults)\n", path);
        return c;
    }
    std::fseek(f, 0, SEEK_END);
    const long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::string text(static_cast<std::size_t>(sz > 0 ? sz : 0), '\0');
    if (!text.empty() && std::fread(text.data(), 1, text.size(), f) != text.size()) {
        std::fclose(f);
        std::fprintf(stderr, "cfg: failed to read %s (using defaults)\n", path);
        return c;
    }
    std::fclose(f);

    const char* p = text.c_str();
    const Json root = parse_value(p);
    if (root.type != Json::Obj) {
        std::fprintf(stderr, "cfg: bad JSON in %s (using defaults)\n", path);
        return c;
    }

    if (const Json* pats = find_obj(root, "patterns"); pats && pats->type == Json::Arr) {
        for (const Json& e : pats->arr) {
            if (e.type != Json::Obj) continue;
            PatternCfg pc;
            if (const Json* v = find_obj(e, "name")) pc.name = v->str;
            if (const Json* v = find_obj(e, "pattern")) pc.pattern = v->str;
            if (const Json* v = find_obj(e, "off")) pc.off = static_cast<int>(v->num);
            if (const Json* v = find_obj(e, "op")) pc.op = v->str;
            if (const Json* v = find_obj(e, "size")) pc.size = static_cast<int>(v->num);
            if (!pc.name.empty() && !pc.pattern.empty()) c.patterns.push_back(std::move(pc));
        }
    }
    if (const Json* req = find_obj(root, "required"); req && req->type == Json::Arr)
        for (const Json& e : req->arr)
            if (e.type == Json::Str) c.required.push_back(e.str);

    if (const Json* offs = find_obj(root, "offsets"); offs && offs->type == Json::Obj) {
        auto set = [&](const char* k, std::uintptr_t& dst) {
            if (const Json* v = find_obj(*offs, k); v && v->type == Json::Str)
                dst = parse_hex(v->str);
        };
        set("dwCSGOInput", c.offsets.dwCSGOInput);
        set("viewAngleOffset", c.offsets.viewAngleOffset);
        set("m_vecViewOffset", c.offsets.m_vecViewOffset);
        set("m_ArmorValue", c.offsets.m_ArmorValue);
        set("m_iIDEntIndex", c.offsets.m_iIDEntIndex);
        set("m_vecVelocity", c.offsets.m_vecVelocity);
        set("m_flFlashOverlayAlpha", c.offsets.m_flFlashOverlayAlpha);
        set("dwLocalPlayerPawn", c.offsets.dwLocalPlayerPawn);
        if (const Json* v = find_obj(*offs, "sensitivity"); v && v->type == Json::Num)
            c.offsets.sensitivity = static_cast<float>(v->num);
    }

    c.loaded = true;
    std::fprintf(stderr, "cfg: loaded %zu patterns, %zu offsets from %s\n",
                 c.patterns.size(), c.required.size(), path);
    return c;
}
