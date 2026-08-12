#pragma once

#include "game.h"

#include <string>

// Terminal ANSI ESP: top-down radar around the local player plus a sorted
// text list. Works in any terminal emulator, including Wayland-native ones.
class RadarRenderer {
public:
    void render(const Snapshot& snap, const std::string& status_line);
};
