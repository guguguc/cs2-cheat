#include "overlay_draw.h"

#include "config.h"
#include "math.h"
#include "overlay_ctx.h"

#include "imgui.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>

namespace {

ImU32 visibility_color(bool visible) {
    // visible = red, hidden = blue
    return visible ? IM_COL32(255, 60, 60, 255) : IM_COL32(70, 130, 255, 255);
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

Overlay g_overlay;

void Overlay::esp(SharedCtx& ctx, const ImVec2& display) const {
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

        const ImU32 col = visibility_color(p.visible);

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

        // ---- head circle (deadlocked: radius from spine3->neck height) ----
        if (ctx.head_circle && p.bone_pos.size() > 6 && p.bone_pos.size() > 4) {
            Vector2 neck, spine3;
            if (WorldToScreen(p.bone_pos[6], snap.view_matrix,
                              static_cast<int>(sw), static_cast<int>(sh), neck) &&
                WorldToScreen(p.bone_pos[4], snap.view_matrix,
                              static_cast<int>(sw), static_cast<int>(sh), spine3)) {
                const float h = spine3.y - neck.y;
                if (h > 1.f) {
                    const ImVec2 c{neck.x - (spine3.x - neck.x) * 0.5f,
                                   neck.y - h * 0.5f};
                    dl->AddCircle(c, h * 0.5f, col, 0, 1.5f);
                }
            }
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

void Overlay::aim_hint(SharedCtx& ctx, const ImVec2& display) const {
    if (!ctx.aim_active || ctx.panel_open) return;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const int sw = static_cast<int>(display.x);
    const int sh = static_cast<int>(display.y);

    // Translucent box from the crosshair (screen center) toward the locked
    // target's screen position; updates every frame as the aimbot steers.
    Vector2 tscr;
    if (WorldToScreen(ctx.aim_target_world, ctx.snap.view_matrix, sw, sh, tscr)) {
        const ImVec2 c{display.x * 0.5f, display.y * 0.5f};
        const ImVec2 lo{std::min(c.x, tscr.x), std::min(c.y, tscr.y)};
        const ImVec2 hi{std::max(c.x, tscr.x), std::max(c.y, tscr.y)};
        dl->AddRectFilled(lo, hi, IM_COL32(0, 255, 120, 46));
        dl->AddRect(lo, hi, IM_COL32(0, 255, 120, 160), 0.f, 0, 1.5f);
    }

    // Big transparent text, centered just below the crosshair.
    const char* text = "AIMBOT ON";
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

// --- vector tab icons (no font glyphs needed; theme-colored) -------------
void draw_icon_crosshair(ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
    dl->AddLine({c.x - r, c.y}, {c.x + r, c.y}, col, 1.8f);
    dl->AddLine({c.x, c.y - r}, {c.x, c.y + r}, col, 1.8f);
    dl->AddCircleFilled(c, 1.6f, col);
}
void draw_icon_eye(ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
    const int seg = 24;
    std::vector<ImVec2> top, bot;
    for (int i = 0; i <= seg; ++i) {
        const float t = 3.14159265f * static_cast<float>(i) / static_cast<float>(seg);
        top.push_back({c.x + r * std::cos(t) * 1.3f, c.y - r * std::sin(t) * 0.55f});
        bot.push_back({c.x - r * std::cos(t) * 1.3f, c.y + r * std::sin(t) * 0.55f});
    }
    dl->AddPolyline(top.data(), static_cast<int>(top.size()), col, 0, 1.8f);
    dl->AddPolyline(bot.data(), static_cast<int>(bot.size()), col, 0, 1.8f);
    dl->AddCircleFilled(c, r * 0.30f, col);
}
void draw_icon_target(ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
    dl->AddCircle(c, r, col, 24, 1.8f);
    dl->AddCircle(c, r * 0.55f, col, 24, 1.5f);
    dl->AddCircleFilled(c, 1.6f, col);
}
void draw_icon_gear(ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
    dl->AddCircle(c, r * 0.6f, col, 24, 2.0f);
    for (int i = 0; i < 8; ++i) {
        const float a = 6.2831853f * static_cast<float>(i) / 8.f;
        const ImVec2 p1{c.x + std::cos(a) * r * 0.95f, c.y + std::sin(a) * r * 0.95f};
        const ImVec2 p2{c.x + std::cos(a) * r * 1.35f, c.y + std::sin(a) * r * 1.35f};
        dl->AddLine(p1, p2, col, 2.0f);
    }
    dl->AddCircleFilled(c, 1.6f, col);
}

// iOS-style switch: rounded track + sliding knob, animated. Returns true when
// the user clicks it. Keeps ImGui state via an invisible button.
bool ui_toggle(const char* label, bool* v) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetFrameHeight();
    const float w = h * 1.9f;
    const float radius = h * 0.5f;
    ImGui::InvisibleButton(label, ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    const bool clicked = ImGui::IsItemClicked();
    if (clicked) *v = !*v;

    // Animated knob position (ease toward target). Each toggle keeps its own
    // animation state in ImGui's per-window storage, keyed by its label ID -
    // a shared static would make every switch move together.
    const float target = *v ? 1.f : 0.f;
    constexpr float kAnim = 0.35f;  // per-frame blend
    ImGuiID id = ImGui::GetID(label);
    float* animp = ImGui::GetStateStorage()->GetFloatRef(id, 0.f);
    float& anim = *animp;
    anim += (target - anim) * kAnim;
    if (anim < 0.001f) anim = 0.f;
    if (anim > 0.999f) anim = 1.f;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Track: green when on, grey when off.
    const ImU32 track = *v ? IM_COL32(0x00, 0xA8, 0x6B, 255)  // deep emerald #00A86B
                           : (hovered ? IM_COL32(120, 120, 125, 150)
                                      : IM_COL32(95, 95, 100, 130));
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), track, radius);
    // Knob with a subtle border.
    const float pad = 2.f;
    const float kx = p.x + pad + anim * (w - 2.f * radius);
    dl->AddCircleFilled(ImVec2(kx + radius - pad, p.y + radius), radius - pad,
                        IM_COL32(255, 255, 255, 255));
    dl->AddCircle(ImVec2(kx + radius - pad, p.y + radius), radius - pad,
                  IM_COL32(0, 0, 0, 60), 0, 1.f);

    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    return clicked;
}

void Overlay::panel(SharedCtx& ctx) const {
    if (!ctx.panel_open) return;

    ImGui::SetNextWindowPos({16.f, 16.f}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(560.f, 420.f), ImGuiCond_FirstUseEver);
    ImGui::Begin("cs2-internal", nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoResize);  // fixed size, no title bar

    // ---- header ----
    ImGui::PushFont(ImGui::GetIO().Fonts->Fonts.empty() ? nullptr
                     : ImGui::GetIO().Fonts->Fonts.back());
    ImGui::TextColored(ImVec4(0.25f, 0.95f, 0.60f, 1.f), "CS2 INTERNAL");
    ImGui::PopFont();
    ImGui::TextDisabled("cheat menu · F1 to close");
    ImGui::Spacing();

    // ---- left vertical nav (theme-green selection) + content ----
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.f, 0.f, 0.f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.f, 1.f, 1.f, 0.08f));
    ImGui::BeginChild("nav", ImVec2(150.f, ImGui::GetContentRegionAvail().y), true);
    const char* tabs[] = {"AIM", "ESP", "TRIGGER", "SETTINGS"};
    const float nav_w = ImGui::GetContentRegionAvail().x;
    for (int i = 0; i < 4; ++i) {
        const bool sel = (i == ctx.ui_tab);
        const ImVec2 cpos = ImGui::GetCursorScreenPos();
        const float row_h = ImGui::GetFrameHeight() + 6.f;
        ImGui::InvisibleButton(tabs[i], ImVec2(nav_w, row_h));
        const bool hovered = ImGui::IsItemHovered();
        const bool clicked = ImGui::IsItemClicked();
        if (clicked) ctx.ui_tab = i;

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 text_col = sel ? IM_COL32(0x40, 0xF2, 0x99, 255)
                                   : (hovered ? IM_COL32(0.95f*255, 0.96f*255, 1.0f*255, 255)
                                              : IM_COL32(0xE0, 0xE4, 0xEA, 220));
        if (sel) {
            // rounded green-tinted selection background
            dl->AddRectFilled(cpos, ImVec2(cpos.x + nav_w, cpos.y + row_h),
                              IM_COL32(0x40, 0xF2, 0x99, 26), 6.f);
            // left indicator bar
            dl->AddRectFilled(ImVec2(cpos.x, cpos.y + 3.f),
                              ImVec2(cpos.x + 3.f, cpos.y + row_h - 3.f),
                              IM_COL32(0x40, 0xF2, 0x99, 255), 1.5f);
        } else if (hovered) {
            dl->AddRectFilled(cpos, ImVec2(cpos.x + nav_w, cpos.y + row_h),
                              IM_COL32(255, 255, 255, 10), 6.f);
        }
        // vector icon + label
        const ImVec2 ic{cpos.x + 14.f, cpos.y + row_h * 0.5f};
        const float ir = 7.f;
        if (tabs[i][0] == 'A') draw_icon_crosshair(dl, ic, ir, text_col);
        else if (tabs[i][0] == 'E') draw_icon_eye(dl, ic, ir, text_col);
        else if (tabs[i][0] == 'T') draw_icon_target(dl, ic, ir, text_col);
        else draw_icon_gear(dl, ic, ir, text_col);
        dl->AddText(ImVec2(cpos.x + 30.f, cpos.y + 3.f), text_col, tabs[i]);
        ImGui::Dummy(ImVec2(0.f, 4.f));
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(2);

    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.f, 0.f, 0.f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.f, 1.f, 1.f, 0.08f));
    ImGui::BeginChild("content", ImVec2(390.f, ImGui::GetContentRegionAvail().y), true);
    ImGui::SetCursorPos(ImVec2(12.f, 10.f));  // padding from the content border

    // ------------------------------ AIM ------------------------------
    if (ctx.ui_tab == 0) {
        {
            const bool prev = ctx.aim_on;
            ui_toggle("Aimbot", &ctx.aim_on);
            if (ctx.aim_on != prev) ctx.aim_toggle = ctx.aim_on;
        }
        if (ctx.aim_on) {
            ui_toggle("Visibility check", &ctx.visibility_check);
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextDisabled("AIM SETTINGS");
        ImGui::SliderFloat("FOV (deg)", &ctx.aim_fov, 1.f, 60.f, "%.1f");
        ImGui::SliderFloat("Smooth", &ctx.aim_smooth, 1.f, 30.f, "%.1f");
        ImGui::SliderFloat("Max dist (m)", &ctx.esp_max_dist, 10.f, 400.f, "%.0f");
    }
    // ------------------------------ ESP ------------------------------
    else if (ctx.ui_tab == 1) {
        ui_toggle("ESP", &ctx.esp_on);
        if (ctx.esp_on) {
            ui_toggle("Head circle", &ctx.head_circle);
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextDisabled("ESP SETTINGS");
        ImGui::SliderFloat("Max dist (m)", &ctx.esp_max_dist, 10.f, 400.f, "%.0f");
    }
    // ---------------------------- TRIGGER ----------------------------
    else if (ctx.ui_tab == 2) {
        ui_toggle("Triggerbot", &ctx.trigger_on);
        if (ctx.trigger_on) {
            ui_toggle("Head only", &ctx.trigger_head_only);
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextDisabled("RECOIL CONTROL (RCS)");
        ui_toggle("RCS", &ctx.rcs_on);
        if (ctx.rcs_on) {
            ImGui::SliderFloat("Strength", &ctx.rcs_strength, 0.f, 1.f, "%.2f");
        }
    }
    // ---------------------------- SETTINGS ---------------------------
    else {
        const bool in_match = ctx.valid;
        const char* status = in_match ? "IN MATCH" : "MENU / LOBBY";
        const ImVec4 status_col = in_match ? ImVec4(0.25f, 0.95f, 0.60f, 1.f)
                                           : ImVec4(0.95f, 0.60f, 0.25f, 1.f);
        ImGui::TextColored(status_col, "● %s", status);
        int enemies = 0;
        for (const Player& p : ctx.snap.players) {
            if (p.valid && p.alive && p.team != ctx.snap.local.team) ++enemies;
        }
        ImGui::TextDisabled("enemies: %d", enemies);
        ImGui::TextDisabled("aim: %s", ctx.aim_active ? "locking" : "idle");
    }

    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::End();
}
