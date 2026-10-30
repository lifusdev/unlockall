#include "lifus/unlockall/log/Internal.h"
#include <filesystem>
#include <regex>

namespace uwu::diagnostics {
retention prune(const std::wstring& directory_value) noexcept {
    retention result_value;
    try {
        const auto folder = std::filesystem::absolute(directory_value).lexically_normal();
        const std::wregex owned(
            LR"(^unlockall-diagnostic-[0-9]{8}T[0-9]{6}-[0-9]{3}Z-[0-9]+-[0-9]+\.(jsonl|zip)$)");
        for (const auto& entry_value : std::filesystem::directory_iterator(folder)) {
            const auto path_value = entry_value.path().lexically_normal();
            if (path_value.parent_path() != folder ||
                !std::regex_match(path_value.filename().wstring(), owned))
                continue;
            const auto flags = GetFileAttributesW(path_value.c_str());
            if (flags == INVALID_FILE_ATTRIBUTES ||
                flags & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
                continue;
            if (DeleteFileW(path_value.c_str()))
                ++result_value.removed;
            else if (!result_value.error)
                result_value.error = GetLastError();
        }
    } catch (const std::filesystem::filesystem_error& error_value) {
        result_value.error = static_cast<DWORD>(error_value.code().value());
    } catch (...) {
        result_value.error = ERROR_NOT_ENOUGH_MEMORY;
    }
    return result_value;
}
}
