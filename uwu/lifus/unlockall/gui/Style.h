#pragma once
#include <imgui.h>

namespace uwu::theme {
constexpr ImU32 background = IM_COL32(9, 9, 9, 255);
constexpr ImU32 content_bg = IM_COL32(14, 14, 14, 255);
constexpr ImU32 surface = IM_COL32(245, 245, 245, 255);
constexpr ImU32 surface2 = IM_COL32(22, 22, 22, 255);
constexpr ImU32 surface3 = IM_COL32(36, 36, 36, 255);
constexpr ImU32 surface_hover = IM_COL32(47, 47, 47, 255);
constexpr ImU32 surface_active = IM_COL32(62, 62, 62, 255);
constexpr ImU32 border = IM_COL32(51, 51, 51, 255);
constexpr ImU32 text_primary = IM_COL32(244, 244, 244, 255);
constexpr ImU32 text_secondary = IM_COL32(190, 190, 190, 255);
constexpr ImU32 text_muted = IM_COL32(120, 120, 120, 255);
constexpr ImU32 accent = IM_COL32(240, 240, 240, 255);
constexpr ImU32 accent_soft = IM_COL32(44, 44, 44, 255);
constexpr ImU32 accent_light = IM_COL32(255, 255, 255, 255);

constexpr float panel_width = 640.0f;
constexpr float panel_height = 380.0f;
constexpr float sidebar_width = 100.0f;
constexpr float header_height = 48.0f;
constexpr float content_top = 60.0f;
constexpr float pad = 14.0f;
constexpr float rounding = 7.0f;
constexpr float rounding_box = 5.0f;
constexpr float row_height = 26.0f;
constexpr float row_gap = 2.0f;
constexpr float content_width = panel_width - sidebar_width - pad * 2.0f;
constexpr float content_height = panel_height - content_top - pad - 4.0f;

inline void apply() {
    ImGui::StyleColorsDark();
    auto& ui = ImGui::GetStyle();
    for (auto& color : ui.Colors) {
        const float gray = (color.x + color.y + color.z) / 3.0f;
        color.x = color.y = color.z = gray;
    }
    ui.WindowRounding = rounding;
    ui.ChildRounding = ui.PopupRounding = ui.FrameRounding = rounding_box;
    ui.GrabRounding = ui.ScrollbarRounding = ui.TabRounding = rounding_box;
    ui.WindowBorderSize = 0.0f;
    ui.PopupBorderSize = 1.0f;
    ui.FrameBorderSize = 0.0f;
    ui.WindowPadding = {0.0f, 0.0f};
    ui.ItemSpacing = {8.0f, row_gap};
    const auto set_value = [&](ImGuiCol slot, ImU32 color) {
        ui.Colors[slot] = ImGui::ColorConvertU32ToFloat4(color);
    };
    set_value(ImGuiCol_WindowBg, background);
    ui.Colors[ImGuiCol_ChildBg] = {0, 0, 0, 0};
    set_value(ImGuiCol_PopupBg, surface2);
    set_value(ImGuiCol_Text, text_primary);
    set_value(ImGuiCol_TextDisabled, text_muted);
    set_value(ImGuiCol_FrameBg, surface2);
    set_value(ImGuiCol_FrameBgHovered, surface_hover);
    set_value(ImGuiCol_FrameBgActive, surface_active);
    set_value(ImGuiCol_Border, border);
    set_value(ImGuiCol_Button, surface3);
    set_value(ImGuiCol_ButtonHovered, surface_hover);
    set_value(ImGuiCol_ButtonActive, accent);
    set_value(ImGuiCol_CheckMark, accent);
    set_value(ImGuiCol_SliderGrab, accent);
    set_value(ImGuiCol_SliderGrabActive, accent_light);
    set_value(ImGuiCol_Header, accent_soft);
    set_value(ImGuiCol_HeaderHovered, surface_hover);
    set_value(ImGuiCol_HeaderActive, accent);
    set_value(ImGuiCol_Separator, border);
    set_value(ImGuiCol_TextSelectedBg, accent_soft);
}
}
