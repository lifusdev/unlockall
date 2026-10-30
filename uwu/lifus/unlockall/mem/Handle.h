#pragma once
#include <windows.h>
#include <utility>

namespace uwu {
class unique_handle {
  public:
    unique_handle() = default;

    explicit unique_handle(HANDLE value_data) noexcept : value_(value_data) {}

    ~unique_handle() { reset(); }

    unique_handle(const unique_handle&) = delete;
    unique_handle& operator=(const unique_handle&) = delete;

    unique_handle(unique_handle&& other) noexcept : value_(other.release()) {}

    unique_handle& operator=(unique_handle&& other) noexcept {
        if (this != &other)
            reset(other.release());
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept { return value_; }

    [[nodiscard]] explicit operator bool() const noexcept { return value_ && value_ != INVALID_HANDLE_VALUE; }

    HANDLE release() noexcept { return std::exchange(value_, nullptr); }

    void reset(HANDLE value_data = nullptr) noexcept {
        if (*this)
            CloseHandle(value_);
        value_ = value_data;
    }

  private:
    HANDLE value_ = nullptr;
};
}
