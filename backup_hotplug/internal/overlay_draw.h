#pragma once

struct ImVec2;
struct SharedCtx;

namespace overlay_draw {

// Draws the ESP boxes/health/labels for the current snapshot.
void esp(SharedCtx& ctx, const ImVec2& display);
// Draws the control panel (visible when ctx.panel_open).
void panel(SharedCtx& ctx);

}  // namespace overlay_draw
