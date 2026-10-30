#pragma once
#include "lifus/unlockall/mem/Handle.h"
#include <cstddef>
#include <array>
#include <utility>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace uwu {
struct process_info {
    DWORD pid;
    std::wstring name;
};

struct module_info {
    std::wstring name;
    std::uintptr_t address;
    std::size_t size;
};

struct thread_entry {
    DWORD tid;
    std::uintptr_t teb;
};

struct read_result {
    std::size_t transferred = 0;
    DWORD error = ERROR_SUCCESS;

    [[nodiscard]] bool complete() const noexcept { return error == ERROR_SUCCESS; }
};

struct read_statistics {
    std::uint64_t calls{}, requested{}, transferred{}, failures{};
    DWORD last_error{};
    std::array<std::pair<DWORD, std::uint64_t>, 16> errors{};
};

[[nodiscard]] std::wstring error_text(DWORD error_value);
[[nodiscard]] std::vector<process_info> processes(DWORD& error_value);
[[nodiscard]] std::optional<DWORD> game_pid();

class memory {
  public:
    [[nodiscard]] DWORD attach(DWORD pid_value) noexcept;

    void detach() noexcept {
        driver_.reset();
        pid_ = 0;
    }

    [[nodiscard]] DWORD pid() const noexcept { return pid_; }

    [[nodiscard]] bool attached() const noexcept { return static_cast<bool>(driver_); }

    [[nodiscard]] read_statistics statistics() const noexcept { return statistics_; }

    [[nodiscard]] read_result read_bytes(
        std::uintptr_t address_value, std::span<std::byte> output) const noexcept;
    [[nodiscard]] DWORD write_bytes(
        std::uintptr_t address_value, std::span<const std::byte> input_value) const noexcept;

    template <class t>
        requires std::is_trivially_copyable_v<t>
    [[nodiscard]] std::optional<t> read(
        std::uintptr_t address_value, DWORD* error_value = nullptr) const noexcept {
        t value_data{};
        const auto result_value =
            read_bytes(address_value, std::as_writable_bytes(std::span<t>(&value_data, 1)));
        if (error_value)
            *error_value = result_value.error;
        if (!result_value.complete())
            return std::nullopt;
        return value_data;
    }

    [[nodiscard]] std::vector<module_info> modules(DWORD& error_value) const;
    [[nodiscard]] std::optional<bool> is64_bit(DWORD& error_value) const noexcept;
    [[nodiscard]] std::vector<thread_entry> threads(DWORD& error_value) const;

  private:
    unique_handle driver_;
    DWORD pid_ = 0;
    mutable read_statistics statistics_;
};
}
