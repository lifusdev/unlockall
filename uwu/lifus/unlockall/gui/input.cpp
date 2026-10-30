#include "lifus/unlockall/gui/Ui.h"
#include <imgui_impl_win32.h>
#include <windowsx.h>
#include <cmath>
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace uwu {
auto interaction::owns(HWND window) const -> bool {
    DWORD pid_value{};
    if (window)
        GetWindowThreadProcessId(window, &pid_value);
    return game_pid && pid_value == game_pid;
}

auto interaction::foreground_game() const -> bool {
    const auto foreground = GetForegroundWindow();
    if (!game || !IsWindowVisible(game) || IsIconic(game))
        return false;
    const auto now = std::chrono::steady_clock::now();
    if (!foreground)
        return now - last_owned < std::chrono::milliseconds(50);
    if (!owns(foreground))
        return false;
    last_owned = now;
    return true;
}

auto interaction::interactive() const -> bool {
    return menu && foreground_game();
}

bool interaction::toggle_menu() {
    menu = !menu;
    return true;
}

auto interaction::release_input() -> void {
    if (GetCapture() == panel)
        ReleaseCapture();
    mouse_down.fill(false);
    mouse_owned.fill(false);
    input_active = false;
    if (!ImGui::GetCurrentContext())
        return;
    ImGui::GetIO().ClearInputKeys();
    ImGui::GetIO().ClearInputMouse();
    ImGui::GetIO().AddFocusEvent(false);
}

auto interaction::sync_input() -> void {
    const bool wanted = interactive();
    if (wanted == input_active)
        return;
    release_input();
    if (!wanted)
        return;
    constexpr int buttons[]{VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};
    for (unsigned button = 0; button < mouse_down.size(); ++button)
        mouse_down[button] = (GetAsyncKeyState(buttons[button]) & 0x8000) != 0;
    input_active = true;
    ImGui::GetIO().AddFocusEvent(true);
}

auto interaction::poll_mouse() -> void {
    if (!input_active)
        return;
    POINT mouse{};
    const bool positioned = GetCursorPos(&mouse) != FALSE;
    const auto hovered_window = positioned ? WindowFromPoint(mouse) : nullptr;
    const bool hovered = hovered_window == panel;
    if (positioned && ScreenToClient(game, &mouse))
        ImGui::GetIO().AddMousePosEvent(static_cast<float>(mouse.x), static_cast<float>(mouse.y));
    constexpr int buttons[]{VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};
    for (unsigned button = 0; button < mouse_down.size(); ++button) {
        const bool down_value = (GetAsyncKeyState(buttons[button]) & 0x8000) != 0;
        if (down_value != mouse_down[button])
            mouse_owned[button] = down_value && hovered;
        mouse_down[button] = down_value;
        ImGui::GetIO().AddMouseButtonEvent(static_cast<int>(button), down_value && mouse_owned[button]);
    }
}

auto interaction::hide() -> void {
    menu_alpha = 0.0f;
    applied = {-1, -1, -1, -1};
    release_input();
    ShowWindow(panel, SW_HIDE);
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    const auto state_value = reinterpret_cast<interaction*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto create = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    if (state_value && window == state_value->panel) {
        if (message == WM_MOUSEACTIVATE)
            return MA_NOACTIVATE;
        if (message == WM_NCHITTEST && !state_value->interactive())
            return HTTRANSPARENT;
        if (message == WM_SETCURSOR && LOWORD(lp) == HTCLIENT && state_value->interactive()) {
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            return TRUE;
        }
        if (message == WM_CANCELMODE || message == WM_CAPTURECHANGED)
            state_value->release_input();
        if (message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_ACTIVATEAPP)
            return 0;
        if (message == WM_MOUSEMOVE || message == WM_NCMOUSEMOVE || message == WM_MOUSELEAVE ||
            message == WM_NCMOUSELEAVE)
            return 0;
        if (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST && message != WM_MOUSEWHEEL &&
            message != WM_MOUSEHWHEEL)
            return 0;
        if (!state_value->interactive() &&
            ((message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) ||
                (message >= WM_KEYFIRST && message <= WM_KEYLAST)))
            return 0;
        if (ImGui_ImplWin32_WndProcHandler(window, message, wp, lp))
            return 1;
    }
    switch (message) {
    case WM_SIZE:
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_SYSCOMMAND:
        if ((wp & 0xfff0) == SC_KEYMENU)
            return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}
}
