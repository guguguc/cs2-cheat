#include "overlay_draw.h"

#include "config.h"
#include "math.h"
#include "overlay_ctx.h"

#include "imgui.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>

namespace {

ImU32 team_color(int team, bool teammate) {
    if (teammate) return IM_COL32(170, 170, 170, 255);
    return team == 2 ? IM_COL32(255, 90, 90, 255) : IM_COL32(90, 190, 255, 255);
}

ImU32 health_color(int hp) {
    if (hp > 60) return IM_COL32(0, 255, 110, 255);
    if (hp > 30) return IM_COL32(255, 235, 60, 255);
    return IM_COL32(255, 60, 60, 255);
}

// CS2 BoneFlags: FlagHitbox = 0x100 (Valthrun model.rs). Only draw bones that participate in
// hitbox checks so the skeleton skips cosmetic/attachment bones (Valthrun
// applies the same filter).
constexpr std::uint32_t kBoneFlagHitbox = 0x100;

}  // namespace

namespace overlay_draw {

void esp(SharedCtx& ctx, const ImVec2& display) {
    if (!ctx.esp_on || !ctx.valid) return;
    const Snapshot& snap = ctx.snap;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const float sw = display.x;
    const float sh = display.y;

    for (const Player& p : snap.players) {
        if (!p.valid || !p.alive) continue;
        if (!cfg::ESP_SHOW_TEAMMATES && p.team == snap.local.team) continue;
        if (ctx.esp_max_dist > 0.f && p.distance_m > ctx.esp_max_dist) continue;

        Vector2 feet, head;
        if (!WorldToScreen(p.feet, snap.view_matrix, static_cast<int>(sw),
                           static_cast<int>(sh), feet))
            continue;
        if (!WorldToScreen(p.head, snap.view_matrix, static_cast<int>(sw),
                           static_cast<int>(sh), head))
            continue;

        const ImU32 col = team_color(p.team, p.team == snap.local.team);

        // ---- skeleton: parent -> child bone lines ----
        const std::size_t n = std::min(p.bone_parents.size(), p.bone_pos.size());
        for (std::size_t i = 0; i < n; ++i) {
            if (i < p.bone_flags.size() &&
                (p.bone_flags[i] & kBoneFlagHitbox) == 0)
                continue;  // only hitbox bones (same filter as Valthrun)
            const int parent = p.bone_parents[i];
            if (parent < 0 || parent >= static_cast<int>(n)) continue;
            Vector2 a, b;
            if (!WorldToScreen(p.bone_pos[parent], snap.view_matrix,
                               static_cast<int>(sw), static_cast<int>(sh), a))
                continue;
            if (!WorldToScreen(p.bone_pos[i], snap.view_matrix,
                               static_cast<int>(sw), static_cast<int>(sh), b))
                continue;
            dl->AddLine({a.x, a.y}, {b.x, b.y}, col, 1.2f);
        }

        const float box_h = feet.y - head.y;
        if (box_h < 8.f) continue;
        const float box_w = std::max(4.f, box_h * 0.55f);
        const ImVec2 a{feet.x - box_w * 0.5f, head.y};
        const ImVec2 b{feet.x + box_w * 0.5f, feet.y};

        dl->AddRect(a, b, col, 0.f, 0, 1.5f);

        const float bar_h = box_h * std::clamp(p.health, 0, 100) / 100.f;
        dl->AddRectFilled({a.x - 5.f, b.y - bar_h}, {a.x - 3.f, b.y},
                          health_color(p.health));

        char buf[64];
        std::snprintf(buf, sizeof(buf), "%d HP  %.0fm", p.health, p.distance_m);
        dl->AddText({b.x + 4.f, a.y}, IM_COL32(255, 255, 255, 235), buf);
    }
}

void aim_hint(SharedCtx& ctx, const ImVec2& display) {
    if (!ctx.aim_active || ctx.panel_open) return;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const char* text = "AIMBOT ON";
    // Big transparent text, centered just below the crosshair.
    ImFont* font = ImGui::GetIO().Fonts->Fonts.empty()
                       ? nullptr
                       : ImGui::GetIO().Fonts->Fonts.back();
    const float size_px = 30.f;  // big, readable at a glance
    const ImVec2 size = font ? font->CalcTextSizeA(size_px, FLT_MAX, 0.f, text)
                             : ImGui::CalcTextSize(text);
    const ImVec2 pos{display.x * 0.5f - size.x * 0.5f,
                     display.y * 0.5f + 46.f};
    if (font)
        dl->AddText(font, size_px, pos, IM_COL32(0, 255, 120, 255), text);
    else
        dl->AddText(pos, IM_COL32(0, 255, 120, 255), text);  // no backdrop
    (void)FLT_MAX;
}

void panel(SharedCtx& ctx) {
    if (!ctx.panel_open) return;
    ImGui::SetNextWindowPos({12.f, 12.f}, ImGuiCond_FirstUseEver);
    ImGui::Begin("cs2-internal", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

    ImGui::Checkbox("ESP", &ctx.esp_on);
    {
        const bool prev = ctx.aim_on;
        ImGui::Checkbox("Aimbot", &ctx.aim_on);
        if (ctx.aim_on != prev) ctx.aim_toggle = ctx.aim_on;  // menu syncs hotkey state
    }
    if (ctx.aim_on)
        ImGui::Checkbox("Visibility check", &ctx.visibility_check);
    ImGui::Checkbox("Triggerbot", &ctx.trigger_on);
    if (ctx.trigger_on)
        ImGui::Checkbox("Head only", &ctx.trigger_head_only);

    ImGui::Separator();
    ImGui::SliderFloat("FOV (deg)", &ctx.aim_fov, 1.f, 60.f, "%.1f");
    ImGui::SliderFloat("Smooth", &ctx.aim_smooth, 1.f, 30.f, "%.1f");
    ImGui::SliderFloat("Max dist (m)", &ctx.esp_max_dist, 10.f, 400.f, "%.0f");

    ImGui::Separator();
    ImGui::TextDisabled("game: %s", ctx.valid ? "connected - in match" : "not in match");
    int enemies = 0;
    for (const Player& p : ctx.snap.players) {
        if (p.valid && p.alive && p.team != ctx.snap.local.team) ++enemies;
    }
    ImGui::TextDisabled("enemies: %d", enemies);
    ImGui::TextDisabled("press F1 to close menu");
    ImGui::End();
}

}  // namespace overlay_draw
