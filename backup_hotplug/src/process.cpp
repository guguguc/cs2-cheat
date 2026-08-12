#include "process.h"

#include <dirent.h>

#include <cstdlib>
#include <algorithm>
#include <fstream>
#include <sstream>

namespace {

std::string read_small_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string strip_newline(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

}  // namespace

std::vector<int> find_processes(const std::string& name) {
    std::vector<int> out;
    DIR* dir = opendir("/proc");
    if (!dir) return out;

    while (dirent* e = readdir(dir)) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;  // numeric only
        const std::string comm =
            strip_newline(read_small_file(std::string("/proc/") + e->d_name + "/comm"));
        if (comm.rfind(name, 0) == 0) {
            out.push_back(std::atoi(e->d_name));
        }
    }
    closedir(dir);

    // Exact comm matches first: `cs2.sh` (Steam launcher script) is a prefix
    // match of `cs2` but is not the game.
    std::stable_sort(out.begin(), out.end(), [&](int a, int b) {
        const bool a_exact = strip_newline(read_small_file("/proc/" + std::to_string(a) + "/comm")) == name;
        const bool b_exact = strip_newline(read_small_file("/proc/" + std::to_string(b) + "/comm")) == name;
        return a_exact && !b_exact;
    });
    return out;
}

std::optional<std::uintptr_t> module_base(int pid, const std::string& module) {
    std::ifstream maps("/proc/" + std::to_string(pid) + "/maps");
    if (!maps) return std::nullopt;

    std::uintptr_t base = 0;
    std::string line;
    while (std::getline(maps, line)) {
        const auto slash = line.rfind('/');
        if (slash == std::string::npos) continue;
        std::string path = line.substr(slash + 1);
        const auto sp = path.find(' ');
        if (sp != std::string::npos) path = path.substr(0, sp);  // strip " (deleted)"
        if (path != module) continue;

        const auto dash = line.find('-');
        if (dash == std::string::npos) continue;
        const std::uintptr_t start =
            std::stoull(line.substr(0, dash), nullptr, 16);
        if (!base || start < base) base = start;
    }
    return base ? std::optional<std::uintptr_t>(base) : std::nullopt;
}

int ptrace_scope() {
    const std::string s =
        strip_newline(read_small_file("/proc/sys/kernel/yama/ptrace_scope"));
    if (s.empty()) return -1;
    return std::atoi(s.c_str());
}
