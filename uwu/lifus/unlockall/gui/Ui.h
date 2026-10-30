#pragma once
#include "lifus/unlockall/gui/Overlay.h"
#include "lifus/unlockall/mem/Handle.h"
#include <imgui.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <string>

namespace uwu {
extern float ui_alpha;
float animate(const char* name_value, float target_value, float speed = 14.0f);
bool action_button(const char* label_value);
void menu_frame(ImVec2 position);

struct interaction {
    HWND panel{}, game{};
    DWORD game_pid{};
    bool menu = true;
    bool panel_centered = false;
    float panel_x = 0.0f, panel_y = 0.0f;
    mutable float menu_alpha = 0.0f;
    mutable RECT applied{-1, -1, -1, -1};
    mutable std::chrono::steady_clock::time_point last_owned{};
    bool input_active{};
    std::array<bool, 5> mouse_down{}, mouse_owned{};

    bool owns(HWND window) const;

    bool foreground_game() const;

    bool interactive() const;

    bool toggle_menu();

    void release_input();

    void sync_input();

    void poll_mouse();

    void hide();
};

struct game_target {
    DWORD pid{};
    HWND window{};
    unique_handle process;
    bool attached{}, closed{};
    std::chrono::steady_clock::time_point next_search{};
    void update();
};

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wp, LPARAM lp);
}
