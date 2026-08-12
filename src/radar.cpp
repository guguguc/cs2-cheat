#include "radar.h"

#include "config.h"
#include "math.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace ansi {
inline constexpr const char* reset  = "\x1b[0m";
inline constexpr const char* bold   = "\x1b[1m";
inline constexpr const char* dim    = "\x1b[2m";
inline constexpr const char* red    = "\x1b[31m";
inline constexpr const char* green  = "\x1b[32m";
inline constexpr const char* yellow = "\x1b[33m";
inline constexpr const char* cyan   = "\x1b[36m";
inline constexpr const char* white  = "\x1b[37m";
}  // namespace ansi

namespace {

struct Cell {
    char c = ' ';
    int color = 0;  // 0 none, 1 red, 2 cyan, 3 green, 4 dim, 5 bold-white
};

const char* color_code(int c) {
    switch (c) {
        case 1: return ansi::red;
        case 2: return ansi::cyan;
        case 3: return ansi::green;
        case 4: return ansi::dim;
        case 5: return ansi::bold;
        default: return "";
    }
}

const char* compass(const Vector2& r) {
    static const char* dirs[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    const double ang = std::atan2(r.x, r.y) * kRad2Deg;  // 0 = forward
    int idx = static_cast<int>(std::llround(ang / 45.0)) % 8;
    if (idx < 0) idx += 8;
    return dirs[idx];
}

}  // namespace

void RadarRenderer::render(const Snapshot& snap, const std::string& status_line) {
    std::string out;
    out += "\x1b[H";
    out += status_line;
    out += "\x1b[K\n\n";

    if (!snap.valid) {
        out += ansi::dim;
        out += snap.connected
                   ? "connected - waiting for local player (in menu?)"
                   : "cs2 process not found; start the game";
        out += ansi::reset;
        out += "\x1b[K\n";
        fputs(out.c_str(), stdout);
        fflush(stdout);
        return;
    }

    const int R = cfg::RADAR_CELLS;
    const int W = 2 * R + 1;
    const double cell_m = cfg::RADAR_METERS_PER_CELL;

    std::vector<std::vector<Cell>> grid(W, std::vector<Cell>(W));

    // Axis lines.
    for (int i = 0; i < W; ++i) {
        grid[R][i].c = '.';  grid[R][i].color = 4;
        grid[i][R].c = '.';  grid[i][R].color = 4;
    }
    // Compass.
    grid[0][R] = {'N', 5};
    grid[W - 1][R] = {'S', 5};
    grid[R][0] = {'W', 5};
    grid[R][W - 1] = {'E', 5};
    grid[0][0] = grid[0][W - 1] = grid[W - 1][0] = grid[W - 1][W - 1] = {'+', 5};
    // Local player (center).
    grid[R][R] = {'+', 3};

    // Enemies (players list is sorted by distance, so nearest claims the cell).
    for (const Player& p : snap.players) {
        if (!p.valid || !p.alive) continue;
        if (!cfg::ESP_SHOW_TEAMMATES && p.team == snap.local.team) continue;

        int cx = R + static_cast<int>(std::lround(p.radar_xy.x / cell_m));
        int cy = R - static_cast<int>(std::lround(p.radar_xy.y / cell_m));
        cx = std::clamp(cx, 0, W - 1);
        cy = std::clamp(cy, 0, W - 1);
        if (grid[cy][cx].c != ' ') continue;

        if (p.team == snap.local.team) {
            grid[cy][cx] = {'o', 4};
        } else {
            grid[cy][cx] = {'E', p.team == 2 ? 1 : 2};
        }
    }

    for (int y = 0; y < W; ++y) {
        for (int x = 0; x < W; ++x) {
            const Cell& cell = grid[y][x];
            if (cell.color) out += color_code(cell.color);
            out += cell.c;
            if (cell.color) out += ansi::reset;
        }
        out += "\x1b[K\n";
    }

    if (cfg::LIST_VIEW) {
        out += "\n";
        int idx = 0;
        for (const Player& p : snap.players) {
            if (!p.valid || !p.alive) continue;
            if (!cfg::ESP_SHOW_TEAMMATES && p.team == snap.local.team) continue;

            ++idx;
            const int hp = std::clamp(p.health, 0, 100);
            const int filled = hp * 20 / 100;
            char bar[21];
            std::memset(bar, '#', filled);
            std::memset(bar + filled, '-', 20 - filled);
            bar[20] = 0;
            const char* hp_color =
                hp > 60 ? ansi::green : (hp > 30 ? ansi::yellow : ansi::red);
            const char* team_color = p.team == 2 ? ansi::red : ansi::cyan;
            const char* team_name = p.team == 2 ? "T" : "CT";
            const char* dir = compass(p.radar_xy);

            char line[256];
            std::snprintf(line, sizeof(line),
                          "%2d %s%-2s%s %s%3d%s HP [%s]  ARM %3d  %6.1fm  %-2s  flash %3.0f%%%s",
                          idx, team_color, team_name, ansi::reset, hp_color, hp,
                          ansi::reset, bar, p.armor, p.distance_m, dir,
                          p.flash_alpha / 2.55f, ansi::reset);
            out += line;
            out += "\x1b[K\n";
        }
    }

    fputs(out.c_str(), stdout);
    fflush(stdout);
}
