#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace uwu::inv {
inline constexpr std::size_t maximum_items = 100000;
inline constexpr std::size_t maximum_bytes = 32 * 1024 * 1024;
inline constexpr std::uint16_t port = 8080;
inline const std::string route_url = "http://127.0.0.1:" + std::to_string(port) + "/inventory";
}
