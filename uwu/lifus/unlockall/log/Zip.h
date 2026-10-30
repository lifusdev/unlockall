#pragma once
#include <windows.h>
#include <memory>
#include <string>
#include <string_view>

namespace uwu::diagnostics {
class archive {
  public:
    archive();
    ~archive();
    DWORD open(const std::wstring& path_value);
    DWORD append(std::string_view text_value) noexcept;
    DWORD flush() noexcept;
    DWORD close() noexcept;
    explicit operator bool() const noexcept;

  private:
    struct impl;
    std::unique_ptr<impl> impl_;
};
}
