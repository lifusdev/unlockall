#pragma once
#include "lifus/unlockall/log/Log.h"

namespace uwu::diagnostics {
struct retention {
    std::uint64_t removed{};
    DWORD error{};
};

retention prune(const std::wstring& directory_value) noexcept;
bool evidence_record(
    std::string_view event, std::initializer_list<field> fields, std::string_view payload = {}) noexcept;
}
