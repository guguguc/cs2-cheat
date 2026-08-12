#pragma once

#include "game.h"
#include "memory.h"

#include <chrono>

// Fires (via ydotool / uinput, the Wayland-standard input synthesis) when the
// crosshair rests on an alive enemy. Compositor-independent.
class Triggerbot {
public:
    void run(Game& game, const Memory& mem, bool enabled);

private:
    void fire() const;
    std::chrono::steady_clock::time_point next_shot_{};
};
