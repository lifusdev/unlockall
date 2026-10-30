#pragma once
#include <filesystem>

namespace uwu {
int update_inventory_from_game(const std::filesystem::path& inventory) noexcept;
}
