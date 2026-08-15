#pragma once

struct ImVec2;
struct SharedCtx;

// ImGui overlay: draws the ESP boxes/health/skeleton, the control panel and
// the aimbot steering hint. Stateless wrapper around ImGui draw calls.
class Overlay {
public:
    // Draws the ESP boxes/health/labels for the current snapshot.
    void esp(SharedCtx& ctx, const ImVec2& display) const;
    // Draws the control panel (visible when ctx.panel_open).
    void panel(SharedCtx& ctx) const;
    // Draws the "aimbot steering" hint (top-right) while aiming at a target.
    void aim_hint(SharedCtx& ctx, const ImVec2& display) const;
};

// The single overlay used by the Vulkan present hook.
extern Overlay g_overlay;
