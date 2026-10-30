#include "lifus/unlockall/mem/Paths.h"
#include <windows.h>
#include <shlobj.h>
#include <array>
#include <memory>
#include <stdexcept>

namespace uwu {
std::filesystem::path paths::executable() {
    std::array<wchar_t, 32768> filename{};
    const auto length = GetModuleFileNameW(nullptr, filename.data(), static_cast<DWORD>(filename.size()));
    if (!length || length >= filename.size())
        throw std::runtime_error("application path unavailable");
    return std::filesystem::path(std::wstring_view(filename.data(), length));
}

std::filesystem::path paths::data_dir() {
    PWSTR directory{};
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &directory)))
        throw std::runtime_error("application data directory unavailable");
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> storage(directory, &CoTaskMemFree);
    auto result = std::filesystem::path(directory) / "unlockall";
    std::filesystem::create_directories(result);
    return result;
}

std::filesystem::path paths::inventory() {
    const auto directory = data_dir() / "inventory";
    std::filesystem::create_directories(directory);
    return directory / "inventory.json";
}
}
