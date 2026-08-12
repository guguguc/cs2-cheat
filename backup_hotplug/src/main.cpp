#include "aimbot.h"
#include "config.h"
#include "demo.h"
#include "game.h"
#include "memory.h"
#include "offsets.h"
#include "patterns.h"
#include "process.h"
#include "radar.h"
#include "trigger.h"
#ifdef USE_IMGUI
#include "overlay_imgui.h"
#endif

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

bool g_running = true;
bool g_esp = true;
bool g_aim = true;
bool g_trig = cfg::TRIGGER_ENABLED_DEFAULT;
// Runtime tuning, editable from the ImGui panel.
float g_aim_fov = static_cast<float>(cfg::AIM_FOV_DEG);
float g_aim_smooth = static_cast<float>(cfg::AIM_SMOOTH);
float g_esp_max_dist = static_cast<float>(cfg::AIM_MAX_DISTANCE_M);

// Puts stdin in non-blocking, raw-ish mode so terminal keys toggle features.
class TermRaw {
public:
    TermRaw() {
        if (!isatty(0)) return;
        if (tcgetattr(0, &old_) != 0) return;
        termios t = old_;
        t.c_lflag &= ~(ICANON | ECHO);
        t.c_cc[VMIN] = 0;
        t.c_cc[VTIME] = 0;
        tcsetattr(0, TCSANOW, &t);
        fcntl(0, F_SETFL, O_NONBLOCK);
        ok_ = true;
    }
    ~TermRaw() {
        if (ok_) tcsetattr(0, TCSANOW, &old_);
    }

private:
    termios old_{};
    bool ok_ = false;
};

void poll_keys() {
    char c;
    while (read(0, &c, 1) == 1) {
        switch (c) {
            case cfg::KEY_ESP: g_esp = !g_esp; break;
            case cfg::KEY_AIM: g_aim = !g_aim; break;
            case cfg::KEY_TRIGGER: g_trig = !g_trig; break;
            case cfg::KEY_QUIT:
            case 'x':
                g_running = false;
                break;
            default: break;
        }
    }
}

std::string status_line(bool demo, int pid, std::uintptr_t client_base) {
    std::string s = "cs2-cheat | ";
    s += std::string("e:ESP ") + (g_esp ? "on" : "off") + "  ";
    s += std::string("a:AIM ") + (g_aim ? "on" : "off") + "  ";
    s += std::string("t:TRIGGER ") + (g_trig ? "on" : "off") + "  ";
    s += "q:quit";

    if (demo) {
        s += " | DEMO FEED";
    } else if (pid > 0) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), " | pid %d  libclient.so 0x%llx",
                      pid, static_cast<unsigned long long>(client_base));
        s += buf;
    } else {
        s += " | waiting for cs2 ...";
    }
    return s;
}

bool has_flag(int argc, char** argv, const char* flag) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], flag) == 0) return true;
    }
    return false;
}

const char* flag_value(int argc, char** argv, const char* flag) {
    for (int i = 1; i < argc - 1; ++i) {
        if (std::strcmp(argv[i], flag) == 0) return argv[i + 1];
    }
    return nullptr;
}

}  // namespace

int main(int argc, char** argv) {
    const bool demo = has_flag(argc, argv, "--demo");

    std::printf("cs2-cheat - CS2 external ESP/aim for the native Linux build\n");
    std::printf("keys: e=ESP  a=aimbot  t=triggerbot  q=quit\n\n");

    const int scope = ptrace_scope();
    if (scope > 0 && geteuid() != 0) {
        std::printf("warning: yama ptrace_scope=%d. process_vm_readv may be denied.\n"
                    "fix:  sudo sysctl kernel.yama.ptrace_scope=0   (or run as root)\n\n",
                    scope);
    }

    TermRaw raw;
    Memory mem;
    Game game;
    RadarRenderer radar;
    Triggerbot trigger;

#ifdef USE_IMGUI
    ImGuiOverlay overlay;
    const bool want_overlay = has_flag(argc, argv, "--overlay");
    const char* platform = flag_value(argc, argv, "--overlay-platform");
    bool overlay_ok = want_overlay && overlay.init(platform);
    if (want_overlay && !overlay_ok) {
        std::printf("note: ImGui overlay unavailable (no GL/display); using terminal radar\n");
    }
#else
    [[maybe_unused]] const bool overlay_ok = false;
#endif

    auto next_frame = std::chrono::steady_clock::now();
    auto next_resolve_attempt = std::chrono::steady_clock::now();

    while (g_running) {
        poll_keys();

        Snapshot snap;
        std::string status;

        if (demo) {
            const double t =
                std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
                    .count();
            snap = make_demo_snapshot(t);
            status = status_line(true, 0, 0);
        } else {
            if (!mem.attached()) {
                const std::vector<int> pids = find_processes(offsets::PROCESS_NAME);
                // The real game is the candidate that has libclient.so mapped
                // (guards against launcher scripts such as cs2.sh).
                for (const int pid : pids) {
                    if (module_base(pid, offsets::CLIENT_MODULE)) {
                        mem.attach(pid);
                        break;
                    }
                }
            }
            if (mem.attached() && !game.attached()) {
                // Resolve offsets from the live binary once (retry if the game
                // was caught mid-update and a pattern was missing).
                if (std::chrono::steady_clock::now() >= next_resolve_attempt) {
                    patterns::Resolved off;
                    if (patterns::resolve(mem.pid(), off)) {
                        game.attach(mem, off);
                    } else {
                        next_resolve_attempt =
                            std::chrono::steady_clock::now() + std::chrono::seconds(3);
                    }
                }
            }

            if (game.attached()) {
                if (game.offsets_resolved()) {
                    game.update(mem);
                    snap = game.snapshot();
                    run_aimbot(game, mem, g_aim, g_aim_fov, g_aim_smooth);
                    trigger.run(game, mem, g_trig);
                    status = status_line(false, mem.pid(), game.client_base());
                } else {
                    status = "cs2 attached - resolving offsets from live binary ...";
                }
            } else {
                status = status_line(false, 0, 0);
            }
        }

#ifdef USE_IMGUI
        if (overlay_ok) {
            const OverlaySettings os{
                &g_esp, &g_aim, &g_trig, &g_aim_fov, &g_aim_smooth, &g_esp_max_dist,
            };
            overlay.render(snap, os);
            if (overlay.should_close()) {
                std::fprintf(stderr, "overlay closed; continuing with terminal radar\n");
                overlay.shutdown();
                overlay_ok = false;
            }
        }
        // With the overlay active the terminal radar is redundant; skip it to
        // keep stdout quiet.
        if (g_esp && !overlay_ok) radar.render(snap, status);
#else
        if (g_esp) radar.render(snap, status);
#endif

        if (!demo && !game.attached()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        } else {
            next_frame += std::chrono::milliseconds(cfg::FRAME_INTERVAL_MS);
            std::this_thread::sleep_until(next_frame);
        }
    }

#ifdef USE_IMGUI
    if (overlay_ok) overlay.shutdown();
#endif
    std::printf("\nbye\n");
    return 0;
}
