#pragma once
#include <filesystem>
#include <string>

namespace uwu::sdk {
int export_inventory_catalog(const std::filesystem::path& output) noexcept;
std::filesystem::path find_item_cache(const std::filesystem::path& executable);
std::string read_item_cache(const std::filesystem::path& directory);
}
