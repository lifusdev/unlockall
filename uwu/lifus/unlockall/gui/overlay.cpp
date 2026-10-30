#include "lifus/unlockall/gui/Ui.h"
#include "lifus/unlockall/gui/Unlock.h"
#include "lifus/unlockall/gui/D3d.h"
#include "lifus/unlockall/gui/Style.h"
#include "lifus/unlockall/gui/Frame.h"
#include "lifus/unlockall/log/Log.h"
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include "Resource.h"

namespace uwu {
int run_overlay(HINSTANCE instance, bool inventory_test, bool reconnect_test) {
    interaction input_value;
    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = window_proc;
    cls.hInstance = instance;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.lpszClassName = L"unlockall";
    if (!RegisterClassExW(&cls))
        return 1;
    input_value.panel =
        CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
            cls.lpszClassName, L"unlockall", WS_POPUP, 20, 20, static_cast<int>(theme::panel_width),
            static_cast<int>(theme::panel_height), nullptr, nullptr, instance, &input_value);
    if (!input_value.panel) {
        UnregisterClassW(cls.lpszClassName, instance);
        return 1;
    }
    gfx renderer;
    if (!renderer.init(input_value.panel)) {
        DestroyWindow(input_value.panel);
        UnregisterClassW(cls.lpszClassName, instance);
        return 1;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    {
        const HRSRC res = FindResourceW(instance, MAKEINTRESOURCEW(font_menu), RT_RCDATA);
        const HGLOBAL mem = res ? LoadResource(instance, res) : nullptr;
        const void* data = mem ? LockResource(mem) : nullptr;
        const DWORD size_value = res ? SizeofResource(instance, res) : 0;
        if (data && size_value > 0) {
            ImFontConfig cfg{};
            cfg.FontDataOwnedByAtlas = false;
            cfg.OversampleH = 3;
            cfg.OversampleV = 2;
            cfg.PixelSnapH = false;
            ImGui::GetIO().Fonts->AddFontFromMemoryTTF(
                const_cast<void*>(data), static_cast<int>(size_value), 15.0f, &cfg);
            ImGui::GetIO().Fonts->AddFontFromMemoryTTF(
                const_cast<void*>(data), static_cast<int>(size_value), 23.0f, &cfg);
        }
    }

    theme::apply();
    const bool platform_ready = ImGui_ImplWin32_Init(input_value.panel);
    const bool renderer_ready =
        platform_ready && ImGui_ImplDX11_Init(renderer.device.Get(), renderer.context.Get());
    if (!renderer_ready) {
        if (platform_ready)
            ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        DestroyWindow(input_value.panel);
        UnregisterClassW(cls.lpszClassName, instance);
        return 1;
    }
    unlock inventory;
    game_target target_value;
    DWORD target_pid{};
    bool done{}, insert_down{}, end_down{}, occluded{};
    int exit_code{};
    std::uint64_t frames{};
    ImDrawData menu_data;
    diagnostics::write("unlock_ui_ready");
    while (!done) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
            if (message.message == WM_QUIT)
                done = true;
        }
        if (done)
            break;
        target_value.update();
        if (target_value.closed)
            break;
        if (target_pid != target_value.pid) {
            target_pid = target_value.pid;
            input_value.game_pid = target_pid;
            input_value.hide();
        }
        if (input_value.game != target_value.window) {
            input_value.hide();
            input_value.game = target_value.window;
        }
        if (inventory_test && target_pid) {
            inventory_test = false;
            inventory.set_enabled(true, target_pid);
            if (reconnect_test) {
                reconnect_test = false;
                inventory.update_and_reconnect(target_pid);
            }
        }
        inventory.poll(target_pid);
        const bool active = input_value.foreground_game();
        const bool insert = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
        const bool end = (GetAsyncKeyState(VK_END) & 0x8000) != 0;
        if (active && insert && !insert_down && input_value.toggle_menu()) {
            if (input_value.menu)
                ClipCursor(nullptr);
            else
                input_value.release_input();
        }
        if (active && end && !end_down)
            done = true;
        insert_down = insert;
        end_down = end;
        if (done)
            break;
        RECT client{};
        POINT origin{};
        if (!active || !GetClientRect(input_value.game, &client) ||
            !ClientToScreen(input_value.game, &origin) || client.right <= 0 || client.bottom <= 0) {
            if (IsWindowVisible(input_value.panel))
                input_value.hide();
            Sleep(10);
            continue;
        }
        input_value.sync_input();
        if (input_value.menu)
            ClipCursor(nullptr);
        const float max_x = std::max(0.0f, static_cast<float>(client.right) - theme::panel_width);
        const float max_y = std::max(0.0f, static_cast<float>(client.bottom) - theme::panel_height);
        if (!input_value.panel_centered) {
            input_value.panel_x = max_x * 0.5f;
            input_value.panel_y = max_y * 0.5f;
            input_value.panel_centered = true;
        }
        input_value.panel_x = std::clamp(input_value.panel_x, 0.0f, max_x);
        input_value.panel_y = std::clamp(input_value.panel_y, 0.0f, max_y);
        const RECT want{origin.x + static_cast<LONG>(input_value.panel_x),
            origin.y + static_cast<LONG>(input_value.panel_y),
            std::min<LONG>(static_cast<LONG>(theme::panel_width), client.right),
            std::min<LONG>(static_cast<LONG>(theme::panel_height), client.bottom)};
        if (std::memcmp(&want, &input_value.applied, sizeof(RECT)) != 0) {
            SetWindowPos(input_value.panel, HWND_TOPMOST, want.left, want.top, want.right, want.bottom,
                SWP_NOACTIVATE);
            input_value.applied = want;
        }
        const bool visible = input_value.menu || input_value.menu_alpha > 0.01f;
        if (visible && !IsWindowVisible(input_value.panel))
            ShowWindow(input_value.panel, SW_SHOWNOACTIVATE);
        else if (!visible && IsWindowVisible(input_value.panel))
            ShowWindow(input_value.panel, SW_HIDE);
        if (visible)
            SetWindowPos(
                input_value.panel, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE);
        if (occluded && renderer.swap_chain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            Sleep(10);
            continue;
        }
        if (!renderer.resize(static_cast<UINT>(want.right), static_cast<UINT>(want.bottom))) {
            exit_code = 1;
            break;
        }
        const auto ready = renderer.wait_frame(input_value.game);
        if (ready == WAIT_FAILED) {
            exit_code = 1;
            break;
        }
        if (ready != WAIT_OBJECT_0)
            continue;
        if (!input_value.foreground_game()) {
            input_value.hide();
            continue;
        }
        const auto started = std::chrono::steady_clock::now();
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::GetIO().DisplaySize = {static_cast<float>(client.right), static_cast<float>(client.bottom)};
        input_value.poll_mouse();
        ImGui::NewFrame();
        const float alpha = input_value.menu ? 1.0f : 0.0f;
        input_value.menu_alpha += (alpha - input_value.menu_alpha) *
            (1.0f - std::exp(-22.0f * std::min(ImGui::GetIO().DeltaTime, 0.05f)));
        if (std::abs(alpha - input_value.menu_alpha) < 0.001f)
            input_value.menu_alpha = alpha;
        if (input_value.menu_alpha > 0.01f) {
            const auto saved_alpha = ImGui::GetStyle().Alpha;
            ui_alpha = ImGui::GetStyle().Alpha = input_value.menu_alpha;
            ImGui::SetNextWindowPos({input_value.panel_x, input_value.panel_y}, ImGuiCond_Always);
            ImGui::SetNextWindowSize({theme::panel_width, theme::panel_height}, ImGuiCond_Always);
            if (ImGui::Begin("##menu", nullptr,
                    ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                        (input_value.menu ? 0 : ImGuiWindowFlags_NoInputs))) {
                const auto position = ImGui::GetWindowPos();
                ImGui::SetCursorPos({0, 0});
                ImGui::InvisibleButton("##drag", {theme::panel_width, theme::header_height});
                if (ImGui::IsItemActive()) {
                    input_value.panel_x =
                        std::clamp(input_value.panel_x + ImGui::GetIO().MouseDelta.x, 0.0f, max_x);
                    input_value.panel_y =
                        std::clamp(input_value.panel_y + ImGui::GetIO().MouseDelta.y, 0.0f, max_y);
                }
                menu_frame(position);
                ImGui::SetCursorPos({theme::sidebar_width + theme::pad, theme::content_top});
                ImGui::BeginChild("##content", {theme::content_width, theme::content_height}, false,
                    ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar |
                        ImGuiWindowFlags_NoScrollWithMouse);
                inventory.menu(target_pid);
                ImGui::EndChild();
            }
            ImGui::End();
            ImGui::GetStyle().Alpha = saved_alpha;
            ui_alpha = 1.0f;
        }
        ImGui::Render();
        if (!input_value.foreground_game()) {
            input_value.hide();
            continue;
        }
        const auto* output = ImGui::GetDrawData();
        menu_data.Clear();
        menu_data.Valid = true;
        menu_data.DisplayPos = {
            static_cast<float>(want.left - origin.x), static_cast<float>(want.top - origin.y)};
        menu_data.DisplaySize = {static_cast<float>(want.right), static_cast<float>(want.bottom)};
        menu_data.FramebufferScale = output->FramebufferScale;
        menu_data.OwnerViewport = output->OwnerViewport;
        for (auto* list : output->CmdLists)
            menu_data.AddDrawList(list);
        constexpr float clear[]{0, 0, 0, 0};
        auto* target = renderer.target.Get();
        renderer.context->OMSetRenderTargets(1, &target, nullptr);
        renderer.context->ClearRenderTargetView(target, clear);
        if (menu_data.TotalVtxCount)
            ImGui_ImplDX11_RenderDrawData(&menu_data);
        const auto result = renderer.swap_chain->Present(0, 0);
        renderer.next_frame = frame_deadline(started, std::chrono::steady_clock::now(), renderer.interval);
        ++frames;
        occluded = result == DXGI_STATUS_OCCLUDED;
        if (FAILED(result) && result != DXGI_ERROR_WAS_STILL_DRAWING) {
            exit_code = 1;
            break;
        }
    }
    inventory.set_enabled(false, target_pid);
    input_value.hide();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    if (IsWindow(input_value.panel))
        DestroyWindow(input_value.panel);
    UnregisterClassW(cls.lpszClassName, instance);
    diagnostics::write("unlock_ui_closed", {{"frames", frames}, {"exit_code", exit_code}});
    return exit_code;
}
}
