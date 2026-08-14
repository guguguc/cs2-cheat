#pragma once

struct ImVec2;
struct SharedCtx;

namespace overlay_draw {

// Draws the ESP boxes/health/labels for the current snapshot.
void esp(SharedCtx& ctx, const ImVec2& display);
// Draws the control panel (visible when ctx.panel_open).
void panel(SharedCtx& ctx);
// Draws the "aimbot steering" hint (top-right) while aiming at a target.
void aim_hint(SharedCtx& ctx, const ImVec2& display);

}  // namespace overlay_draw
