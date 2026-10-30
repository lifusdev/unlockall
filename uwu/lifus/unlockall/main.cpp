#include "lifus/unlockall/gui/Overlay.h"
#include "lifus/unlockall/log/Log.h"
#include "lifus/unlockall/mem/Handle.h"
#include "lifus/unlockall/unlock/Catalog.h"
#include "lifus/unlockall/unlock/Update.h"
#include <shellapi.h>
#include <exception>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR command_line, int) {
    const auto command = std::wstring_view(command_line ? command_line : L"");
    if (command.starts_with(L"--export-catalog") || command.starts_with(L"--update-inventory")) {
        int count{};
        auto** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
        if (!arguments)
            return 1;
        int result = 1;
        if (count == 3 && std::wstring_view(arguments[1]) == L"--export-catalog")
            result = uwu::sdk::export_inventory_catalog(arguments[2]);
        else if (count == 3 && std::wstring_view(arguments[1]) == L"--update-inventory")
            result = uwu::update_inventory_from_game(arguments[2]);
        LocalFree(arguments);
        return result;
    }
    uwu::unique_handle instance_lock(CreateMutexW(nullptr, FALSE, L"Local\\unlockall.runtime"));
    if (!instance_lock || GetLastError() == ERROR_ALREADY_EXISTS)
        return 0;
    uwu::diagnostics::initialize();

    struct diagnostic_run {
        ~diagnostic_run() { uwu::diagnostics::shutdown(); }
    } diagnostic_run;

    try {
        uwu::diagnostics::phase("driver");
        HANDLE transport_probe =
            CreateFileW(L"\\\\.\\lifus", GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (transport_probe == INVALID_HANDLE_VALUE) {
            const auto error = GetLastError();
            uwu::diagnostics::write("driver_device_missing", {{"win32", error}});
            char message[128];
            wsprintfA(message, "running driver unavailable (win32 %lu)", error);
            MessageBoxA(nullptr, message, "unlockall", MB_OK | MB_ICONWARNING);
            return 1;
        }
        CloseHandle(transport_probe);
        uwu::diagnostics::write("driver_device_ready");
        uwu::diagnostics::phase("overlay");
        const auto arguments = std::wstring_view(command_line ? command_line : L"");
        const bool reconnect = arguments == L"--inventory-reconnect-test";
        return uwu::run_overlay(instance, arguments == L"--inventory-test" || reconnect, reconnect);
    } catch (const std::exception& error) {
        uwu::diagnostics::write("application_exception", {{"message", error.what()}});
        MessageBoxA(nullptr, error.what(), "unlockall", MB_OK | MB_ICONERROR);
    } catch (...) {
        uwu::diagnostics::write("application_exception", {{"message", "unknown exception"}});
        MessageBoxW(nullptr, L"startup failed", L"unlockall", MB_OK | MB_ICONERROR);
    }
    return 1;
}
