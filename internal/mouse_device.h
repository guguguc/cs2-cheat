#pragma once

#include <cstdint>

// Aimbot via a virtual mouse (/dev/uinput), like the Linux external cheats do.
// The game reads the virtual device as a real mouse, so view angles are never
// written to memory - sidesteps the runtime-stubbed CreateMove / stale-global
// problems entirely. Works on Wayland (kernel-level device, no X11 needed).
class MouseDevice {
public:
    bool init();                                    // create the virtual mouse
    void set_input_obj(std::uintptr_t input_obj);  // for reading live view angles
    void set_sensitivity_convar(std::uintptr_t addr);  // live "sensitivity" convar (+0x58 = float)
    void set_local_pawn(std::uintptr_t pawn);      // for reading m_flFOVSensitivityAdjust
    void move_to(float target_pitch, float target_yaw);  // steer toward angle
    void move_counts_raw(int dx, int dy);             // raw relative move (RCS)
    float live_sensitivity() const;                   // convar sens x fov multiplier
    void set_fire(bool on);                            // virtual left-click
    void flush();                                      // send a neutral (0,0)+SYN
    bool active() const;                               // device exists right now
    void shutdown();

private:
    void move_counts(int dx, int dy);

    int fd_ = -1;
    std::uintptr_t input_obj_ = 0;
    std::uintptr_t sens_convar_ = 0;  // live "sensitivity" convar (+0x58 = float value)
    std::uintptr_t local_pawn_ = 0;   // for m_flFOVSensitivityAdjust (zoom sens scale)
    bool firing_ = false;
    std::uint64_t move_log_ = 0;
    // deadlocked-style inertia low-pass filter for the mouse movement.
    float inertia_dx_ = 0.f;
    float inertia_dy_ = 0.f;
};

// The single virtual mouse used by the aimbot / triggerbot / RCS.
extern MouseDevice g_mouse;
