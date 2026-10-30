#include "lifus/unlockall/mem/Memory.h"
#include "lifus/unlockall/unlock/Catalog.h"
#include <array>
#include <fstream>
#include <stdexcept>
#include <system_error>

namespace uwu::sdk {
namespace {
std::string game_catalog() {
    const auto pid = game_pid();
    if (!pid || !*pid)
        throw std::runtime_error("a single running game is required");
    unique_handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, *pid));
    if (!process)
        throw std::system_error(GetLastError(), std::system_category(), "game path access");
    std::array<wchar_t, 32768> buffer{};
    DWORD size = static_cast<DWORD>(buffer.size());
    if (!QueryFullProcessImageNameW(process.get(), 0, buffer.data(), &size))
        throw std::system_error(GetLastError(), std::system_category(), "game path lookup");
    const std::filesystem::path executable(std::wstring_view(buffer.data(), size));
    const auto name = executable.filename().wstring();
    if (_wcsicmp(name.c_str(), L"rainbowsix.exe") && _wcsicmp(name.c_str(), L"rainbowsix_vulkan.exe"))
        throw std::runtime_error("catalog target is not the game");
    const auto result = read_item_cache(find_item_cache(executable));
    DWORD status{};
    if (!GetExitCodeProcess(process.get(), &status) || status != STILL_ACTIVE)
        throw std::runtime_error("game closed during catalog export");
    return result;
}
}

int export_inventory_catalog(const std::filesystem::path& output) noexcept {
    auto temporary = output;
    temporary += L".tmp";
    auto error_path = output;
    error_path += L".error";
    try {
        const auto data = game_catalog();
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file.write(data.data(), static_cast<std::streamsize>(data.size())) || !file.flush())
            throw std::runtime_error("catalog output write failed");
        file.close();
        if (!MoveFileExW(
                temporary.c_str(), output.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("catalog output replacement failed");
        std::error_code ignored;
        std::filesystem::remove(error_path, ignored);
        return 0;
    } catch (const std::exception& error) {
        std::ofstream file(error_path, std::ios::binary | std::ios::trunc);
        file << error.what();
    } catch (...) {
    }
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return 1;
}
}
