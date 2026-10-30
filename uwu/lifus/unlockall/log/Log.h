#pragma once
#include <windows.h>
#include <concepts>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <variant>

namespace uwu::diagnostics {
struct field {
    std::string_view name;
    std::variant<std::string_view, std::uint64_t, std::int64_t, double, bool> value;

    field(std::string_view key_value, std::string_view text_value) : name(key_value), value(text_value) {}

    field(std::string_view key_value, const std::string& text_value)
        : field(key_value, std::string_view(text_value)) {}

    field(std::string_view key_value, const char* text_value)
        : field(key_value, std::string_view(text_value ? text_value : "")) {}

    field(std::string_view key_value, bool flag) : name(key_value), value(flag) {}

    template <std::unsigned_integral t>
    field(std::string_view key_value, t number_value)
        : name(key_value), value(static_cast<std::uint64_t>(number_value)) {}

    template <std::signed_integral t>
    field(std::string_view key_value, t number_value)
        : name(key_value), value(static_cast<std::int64_t>(number_value)) {}

    template <std::floating_point t>
    field(std::string_view key_value, t number_value)
        : name(key_value), value(static_cast<double>(number_value)) {}
};

struct limits {
    std::uint64_t records = 8 * 1024 * 1024;
    std::uint64_t evidence = 1024ull * 1024 * 1024;
};

bool initialize(std::wstring_view directory_value = {}, limits limits_value = {}) noexcept;
void shutdown() noexcept;
bool enabled() noexcept;
DWORD error() noexcept;
std::wstring path();
void open_folder() noexcept;
void write(std::string_view event, std::initializer_list<field> fields = {}) noexcept;
void phase(std::string_view name_value, float progress = 0.0f) noexcept;
std::string hex(std::uint64_t value_data);
std::string sha256(std::span<const std::byte> bytes_value) noexcept;
void memory_status(std::string_view reason) noexcept;
void exception(std::string_view stage, const std::exception& error_value) noexcept;
}
