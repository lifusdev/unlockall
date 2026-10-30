#include "lifus/unlockall/mem/Memory.h"
#include "lifus/unlockall/gui/Ui.h"
#include <imgui_impl_win32.h>
#include <windowsx.h>

namespace uwu {
struct game_window_search {
    DWORD pid{};
    HWND window{};
    LONG area{};
};

BOOL CALLBACK find_game_window(HWND window, LPARAM parameter) {
    auto& search = *reinterpret_cast<game_window_search*>(parameter);
    DWORD pid_value{};
    GetWindowThreadProcessId(window, &pid_value);
    if (pid_value != search.pid || !IsWindowVisible(window) || GetWindow(window, GW_OWNER))
        return TRUE;
    RECT rect_value{};
    if (!GetClientRect(window, &rect_value))
        return TRUE;
    const LONG width = rect_value.right - rect_value.left, height = rect_value.bottom - rect_value.top;
    if (width <= 0 || height <= 0 || width > 32768 || height > 32768)
        return TRUE;
    const LONG area = width * height;
    if (area > search.area) {
        search.window = window;
        search.area = area;
    }
    return TRUE;
}

HWND game_window(DWORD pid_value) {
    game_window_search search{pid_value};
    if (pid_value)
        EnumWindows(find_game_window, reinterpret_cast<LPARAM>(&search));
    return search.window;
}

void game_target::update() {
    if (process) {
        const auto state_value = WaitForSingleObject(process.get(), 0);
        if (state_value != WAIT_TIMEOUT) {
            closed = true;
            return;
        }
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < next_search && (!window || IsWindow(window)))
        return;
    next_search = now + std::chrono::milliseconds(500);
    if (!attached || !process) {
        const auto found = game_pid();
        if (!found)
            return;
        if (attached && *found != pid) {
            closed = true;
            return;
        }
        if (!*found)
            return;
        pid = *found;
        process.reset(OpenProcess(SYNCHRONIZE, FALSE, pid));
        attached = true;
    }
    window = game_window(pid);
}

}
