#include "cm_hook.h"

#include "hook_x64.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>

namespace {

void log_(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void log_(const char* fmt, ...) {
    FILE* f = std::fopen("/tmp/cs2_internal.log", "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(f, fmt, ap);
    va_end(ap);
    std::fclose(f);
}

constexpr std::uintptr_t kViewAnglesA = 0x9C;   // input_obj + 0x9C
constexpr std::uintptr_t kViewAnglesB = 0x548;  // input_obj + 0x548
constexpr std::uintptr_t kViewAnglesC = 0x5F0;  // input_obj + 0x5F0
constexpr int kCreateMoveVtIndex = 8;

std::uintptr_t g_input_obj = 0;
std::uintptr_t g_vtable = 0;
std::uintptr_t g_orig_fn = 0;
hook64::Hook g_cm_hook;

volatile float g_pitch = 0.f;
volatile float g_yaw = 0.f;
volatile bool g_active = false;

volatile bool g_testing = false;
std::uintptr_t g_test_until_ms = 0;
volatile std::uint64_t g_calls = 0;

std::uintptr_t now_ms() {
    return static_cast<std::uintptr_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

void write_angles(float pitch, float yaw) {
    if (!g_input_obj) return;
    auto* a = reinterpret_cast<float*>(g_input_obj + kViewAnglesA);
    auto* b = reinterpret_cast<float*>(g_input_obj + kViewAnglesB);
    auto* c = reinterpret_cast<float*>(g_input_obj + kViewAnglesC);
    a[0] = pitch; a[1] = yaw; a[2] = 0.f;
    b[0] = pitch; b[1] = yaw; b[2] = 0.f;
    c[0] = pitch; c[1] = yaw; c[2] = 0.f;
}

}  // namespace

extern "C" bool cm_hook_create_move(void* me, int slot, bool active) {
    g_calls = g_calls + 1;
    bool ret = false;
    if (g_cm_hook.trampoline)
        ret = reinterpret_cast<bool (*)(void*, int, bool)>(g_cm_hook.trampoline)(
            me, slot, active);

    if (g_testing) {
        if (now_ms() < g_test_until_ms) {
            write_angles(0.f, 90.f);
            return ret;
        }
        g_testing = false;
        std::printf("cm_hook: steer test finished\n");
    }

    if (g_active)
        write_angles(g_pitch, g_yaw);
    return ret;
}

namespace cm_hook {

bool install(std::uintptr_t input_obj) {
    if (!input_obj) {
        log_("cm_hook: install: input_obj is null\n");
        return false;
    }
    g_input_obj = input_obj;
    g_vtable = *reinterpret_cast<std::uintptr_t*>(input_obj);
    if (!g_vtable) {
        log_("cm_hook: install: vtable null (input_obj+0 = 0)\n");
        return false;
    }
    // Input::CreateMove. The engine calls it directly (via a vtable wrapper
    // sub_15D7220 -> direct call), so patching the input object's vtable slot
    // never fires; we inline-hook the function itself.
    g_orig_fn = *reinterpret_cast<std::uintptr_t*>(
        g_vtable + static_cast<std::uintptr_t>(kCreateMoveVtIndex) * sizeof(void*));
    if (!g_orig_fn) {
        log_("cm_hook: install: vtable[%d] null (vtable=0x%llx)\n",
             kCreateMoveVtIndex, static_cast<unsigned long long>(g_vtable));
        return false;
    }
    log_("cm_hook: install: input_obj=0x%llx vtable=0x%llx CreateMove=0x%llx\n",
         static_cast<unsigned long long>(input_obj),
         static_cast<unsigned long long>(g_vtable),
         static_cast<unsigned long long>(g_orig_fn));
    // Dump the runtime prologue so we can compare with the on-disk bytes.
    const auto* rt = reinterpret_cast<const std::uint8_t*>(g_orig_fn);
    std::string dump;
    for (int i = 0; i < 24; ++i)
        dump += std::string(i ? " " : "") + "0123456789abcdef"[rt[i] >> 4] +
                "0123456789abcdef"[rt[i] & 15];
    log_("cm_hook: install: runtime prologue: %s\n", dump.c_str());

    // Capture hook64's own stderr messages into the log.
    fflush(stderr);
    const int saved_err = dup(STDERR_FILENO);
    const int cap = open("/tmp/cm_hook_stderr.log",
                         O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (cap >= 0) dup2(cap, STDERR_FILENO);
    const bool hooked = hook64::install(g_cm_hook,
                                        reinterpret_cast<void*>(g_orig_fn),
                                        reinterpret_cast<void*>(&cm_hook_create_move));
    fflush(stderr);
    if (cap >= 0) {
        dup2(saved_err, STDERR_FILENO);
        close(cap);
    }
    close(saved_err);
    if (!hooked) {
        log_("cm_hook: install: hook64::install failed at 0x%llx\n",
             static_cast<unsigned long long>(g_orig_fn));
        std::FILE* sf = std::fopen("/tmp/cm_hook_stderr.log", "r");
        if (sf) {
            char line[256];
            while (std::fgets(line, sizeof line, sf)) {
                std::string s(line);
                while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
                    s.pop_back();
                log_("cm_hook: hook64: %s\n", s.c_str());
            }
            std::fclose(sf);
        }
        return false;
    }
    log_("cm_hook: install: hooked CreateMove at 0x%llx (trampoline=0x%llx)\n",
         static_cast<unsigned long long>(g_orig_fn),
         reinterpret_cast<unsigned long long>(g_cm_hook.trampoline));
    return true;
}

void uninstall() {
    g_cm_hook = {};
    g_orig_fn = 0;
    g_input_obj = 0;
    g_active = false;
    g_testing = false;
}

void set_cmd_angles(float pitch, float yaw, bool active) {
    g_pitch = pitch;
    g_yaw = yaw;
    g_active = active;
}

void steer_test(float seconds) {
    g_test_until_ms = now_ms() +
                      static_cast<std::uintptr_t>(seconds * 1000.f);
    g_testing = true;
}

std::uintptr_t input_obj() { return g_input_obj; }
std::uint64_t calls() { return g_calls; }
std::uint64_t orig_fn() { return g_orig_fn; }
bool active() { return g_active; }

}  // namespace cm_hook
