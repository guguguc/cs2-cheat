#include "trigger.h"

#include "config.h"
#include "offsets.h"

#include <sys/wait.h>
#include <unistd.h>

#include <chrono>

void Triggerbot::run(Game& game, const Memory& mem, bool enabled) {
    if (!enabled || !game.attached() || !game.local_pawn()) return;
    if (!game.local().alive) return;

    const auto now = std::chrono::steady_clock::now();
    if (now < next_shot_) return;

    const auto idx = mem.read<int>(game.local_pawn() + offsets::m_iIDEntIndex);
    if (!idx || *idx <= 0) return;

    const std::uintptr_t ent = game.entity_by_index(mem, *idx);
    if (!ent || ent == game.local_pawn()) return;
    if (game.entity_team(mem, ent) == game.local().team) return;

    const int hp = game.entity_health(mem, ent);
    if (hp <= 0 || hp > 100) return;

    fire();
    next_shot_ = now + std::chrono::milliseconds(cfg::TRIGGER_DELAY_MS);
}

void Triggerbot::fire() const {
    const pid_t pid = fork();
    if (pid == 0) {
        // Child: exec ydotool. Requires the ydotoold daemon (uinput).
        execlp("ydotool", "ydotool", "click", cfg::TRIGGER_BUTTON, nullptr);
        _exit(127);
    }
    // Reap immediately so we never accumulate zombies.
    if (pid > 0) waitpid(pid, nullptr, 0);
}
