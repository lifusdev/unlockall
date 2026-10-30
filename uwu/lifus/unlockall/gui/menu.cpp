#include "lifus/unlockall/gui/Ui.h"
#include "lifus/unlockall/gui/Style.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace uwu {
float ui_alpha = 1.0f;

ImU32 fade(ImU32 c) {
    const unsigned a = static_cast<unsigned>(((c >> IM_COL32_A_SHIFT) & 0xFF) * ui_alpha);
    return (c & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
}

float row_text_y(float top) {
    return top + (theme::row_height - ImGui::GetTextLineHeight()) * 0.5f;
}

float animate(const char* name_value, float target_value, float speed) {
    auto* state_value = ImGui::GetStateStorage();
    const auto id = ImGui::GetID(name_value);
    const float previous = state_value->GetFloat(id, target_value);
    const float t_value = 1.0f - std::exp(-speed * std::min(ImGui::GetIO().DeltaTime, 0.05f));
    const float value_data = previous + (target_value - previous) * t_value;
    const float result_value = std::abs(value_data - target_value) < 0.001f ? target_value : value_data;
    state_value->SetFloat(id, result_value);
    return result_value;
}

ImU32 lerp_color(ImU32 a, ImU32 b, float t_value) {
    t_value = std::clamp(t_value, 0.0f, 1.0f);
    const float r = ((a >> 0) & 0xFF) * (1 - t_value) + ((b >> 0) & 0xFF) * t_value;
    const float g = ((a >> 8) & 0xFF) * (1 - t_value) + ((b >> 8) & 0xFF) * t_value;
    const float bl = ((a >> 16) & 0xFF) * (1 - t_value) + ((b >> 16) & 0xFF) * t_value;
    const float al = ((a >> 24) & 0xFF) * (1 - t_value) + ((b >> 24) & 0xFF) * t_value;
    return IM_COL32((int)r, (int)g, (int)bl, (int)al);
}

void draw_brand(ImDrawList* draw_value, ImVec2 position) {
    const auto& fonts = ImGui::GetIO().Fonts->Fonts;
    auto* font = fonts.Size > 1 ? fonts[1] : ImGui::GetFont();
    const ImVec2 at{position.x + 14, position.y + (theme::header_height - 23.0f) * 0.5f};
    const auto width = font->CalcTextSizeA(23.0f, 1000.0f, 0.0f, "unlockall").x;
    const auto sweep = static_cast<float>(std::fmod(ImGui::GetTime() * 2.0, 1.8) - 0.4);
    const auto shade = [&](int begin_value) {
        for (int i = begin_value; i < draw_value->VtxBuffer.Size; ++i) {
            auto& vertex = draw_value->VtxBuffer[i];
            const auto distance = ((vertex.pos.x - at.x) / std::max(width, 1.0f) - sweep) * 5.0f;
            const auto gray = static_cast<int>(205.0f + 50.0f * std::exp(-distance * distance));
            vertex.col = IM_COL32(gray, gray, gray, 0) | (vertex.col & IM_COL32_A_MASK);
        }
    };
    for (const auto offset : std::array<ImVec2, 4>{{{-1.2f, 0}, {1.2f, 0}, {0, -1.2f}, {0, 1.2f}}}) {
        const auto begin_value = draw_value->VtxBuffer.Size;
        draw_value->AddText(
            font, 23.0f, {at.x + offset.x, at.y + offset.y}, fade(IM_COL32(255, 255, 255, 16)), "unlockall");
        shade(begin_value);
    }
    const auto begin_value = draw_value->VtxBuffer.Size;
    draw_value->AddText(font, 23.0f, at, fade(theme::text_primary), "unlockall");
    shade(begin_value);
    draw_value->AddLine({position.x + 1, position.y + theme::header_height},
        {position.x + theme::panel_width - 1, position.y + theme::header_height}, fade(theme::border));
}

void draw_snow(ImDrawList* draw_value, ImVec2 position) {
    const float width = theme::panel_width - 16.0f, height = theme::panel_height - 16.0f;
    const double time = ImGui::GetTime();
    draw_value->PushClipRect({position.x + 5, position.y + 5},
        {position.x + theme::panel_width - 5, position.y + theme::panel_height - 5}, true);
    for (unsigned i = 0; i < 26; ++i) {
        const auto depth = static_cast<float>((i * 37 + 11) % 101) / 100.0f;
        const auto seed_value = static_cast<float>((i * 79 + 43) % 997) / 997.0f;
        const auto drift = static_cast<float>(std::sin(time * (0.28 + depth * 0.18) + i * 1.7)) * 7.0f;
        const ImVec2 center{position.x + 8.0f + seed_value * width + drift,
            position.y + 8.0f +
                static_cast<float>(std::fmod(time * (14.0 + depth * 20.0) + i * 47.0, height))};
        const auto radius = 1.0f + depth * 1.8f;
        draw_value->AddCircleFilled(center, radius + 1.2f, fade(IM_COL32(240, 240, 240, 5)), 12);
        draw_value->AddCircleFilled(
            center, radius, fade(IM_COL32(235, 235, 235, static_cast<int>(12 + depth * 14))), 12);
    }
    draw_value->PopClipRect();
}

bool action_button(const char* label_value) {
    const auto cursor = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    ImGui::PushID(label_value);
    const bool pressed = ImGui::InvisibleButton("##action", {width, theme::row_height});
    const float hover = animate("hover", ImGui::IsItemHovered() ? 1.0f : 0.0f);
    const float active_value = animate("press", ImGui::IsItemActive() ? 1.0f : 0.0f, 24.0f);
    auto* draw_value = ImGui::GetWindowDrawList();
    const ImVec2 min{cursor.x, cursor.y + active_value};
    const ImVec2 max{cursor.x + width, cursor.y + theme::row_height};
    draw_value->AddRectFilled(
        min, max, fade(lerp_color(theme::surface2, theme::surface_hover, hover)), theme::rounding_box);
    draw_value->AddRect(min, max, fade(theme::border), theme::rounding_box);
    draw_value->AddText(
        {cursor.x + (width - ImGui::CalcTextSize(label_value).x) * 0.5f, row_text_y(cursor.y) + active_value},
        fade(theme::text_primary), label_value);
    ImGui::PopID();
    return pressed;
}

void menu_frame(ImVec2 position) {
    auto* draw_value = ImGui::GetWindowDrawList();
    const ImVec2 panel_max{position.x + theme::panel_width, position.y + theme::panel_height};
    draw_value->AddRectFilled(position, panel_max, fade(theme::background), theme::rounding);
    draw_value->AddRectFilled({position.x + theme::sidebar_width, position.y + theme::header_height},
        panel_max, fade(theme::content_bg), theme::rounding, ImDrawFlags_RoundCornersBottomRight);
    draw_snow(draw_value, position);
    draw_value->AddRect(position, panel_max, fade(theme::border), theme::rounding);
    draw_brand(draw_value, position);
    const float top = theme::content_top;
    draw_value->AddRectFilled({position.x + 6, position.y + top},
        {position.x + theme::sidebar_width - 6, position.y + top + theme::row_height}, fade(theme::accent),
        theme::rounding_box);
    draw_value->AddText(
        {position.x + 13, position.y + row_text_y(top)}, fade(theme::background), "unlock all");
}
}
