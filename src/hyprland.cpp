#include "hyprland.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

std::string run_out(const std::string& cmd) {
    std::string out;
    FILE* f = popen(cmd.c_str(), "r");
    if (!f) return out;
    char buf[4096];
    std::size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    pclose(f);
    return out;
}

int run_rc(const std::string& cmd) {
    const int rc = std::system((cmd + " >/dev/null 2>&1").c_str());
    return rc;
}

// The `hyprctl clients` plain-text format is a series of blocks:
//   Window <addr> -> <title>:
//       mapped: 1
//       ...
//       class: cs2
//       at: 0,0
//       size: 2560,1440
//       title: Counter-Strike 2
std::string trim_left(std::string s) {
    s.erase(0, s.find_first_not_of(" \t"));
    return s;
}

}  // namespace

namespace hypr {

bool available() {
    if (const char* env = std::getenv("XDG_CURRENT_DESKTOP"); env &&
        std::strstr(env, "Hyprland") != nullptr)
        return true;
    return !run_out("hyprctl version").empty();
}

bool find_game_window(int& x, int& y, int& w, int& h) {
    const std::string out = run_out("hyprctl clients");

    auto scan_block = [&](const std::string& block, int& bx, int& by, int& bw,
                          int& bh) -> bool {
        bool game = false, geo = false;
        std::size_t pos = 0;
        while (pos < block.size()) {
            const std::size_t e = block.find('\n', pos);
            const std::string line =
                block.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
            pos = e == std::string::npos ? block.size() : e + 1;

            const std::size_t colon = line.find(':');
            if (colon == std::string::npos) continue;
            const std::string key = trim_left(line.substr(0, colon));
            std::string val = line.substr(colon + 1);
            while (!val.empty() && val.front() == ' ') val.erase(0, 1);

            if (key == "class" && val == "cs2") game = true;
            else if (key == "at") std::sscanf(val.c_str(), "%d,%d", &bx, &by);
            else if (key == "size")
                geo = std::sscanf(val.c_str(), "%d,%d", &bw, &bh) == 2;
        }
        if (game && geo) return true;
        return false;
    };

    std::string block;
    std::size_t pos = 0;
    while (pos < out.size()) {
        const std::size_t e = out.find('\n', pos);
        const std::string line =
            out.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
        pos = e == std::string::npos ? out.size() : e + 1;
        if (line.rfind("Window ", 0) == 0) {
            if (scan_block(block, x, y, w, h)) return true;
            block.clear();
        }
        block += line + "\n";
    }
    return scan_block(block, x, y, w, h);
}

bool place_overlay(int x, int y, int w, int h) {
    // Single-quote the selector: the title contains characters that the shell
    // would otherwise interpret (parentheses).
    const std::string sel = "'title:^(cs2-cheat overlay)$'";
    const std::string xs = std::to_string(x), ys = std::to_string(y);
    const std::string ws = std::to_string(w), hs = std::to_string(h);
    bool ok = true;
    // Moving to a special workspace floats the window automatically.
    ok &= run_rc("hyprctl dispatch movetoworkspacesilent special:cs2cheat," + sel) == 0;
    // Hyprland's resizewindowpixel re-centers, so pin position after resize too.
    ok &= run_rc("hyprctl dispatch movewindowpixel exact " + xs + " " + ys + "," + sel) == 0;
    ok &= run_rc("hyprctl dispatch resizewindowpixel exact " + ws + " " + hs + "," + sel) == 0;
    ok &= run_rc("hyprctl dispatch movewindowpixel exact " + xs + " " + ys + "," + sel) == 0;
    return ok;
}

}  // namespace hypr
