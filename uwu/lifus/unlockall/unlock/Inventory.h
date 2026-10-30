#pragma once
#include <filesystem>
#include <string>

namespace uwu {
struct inventory {
    std::string body;
    std::string health;
    std::size_t items{};
    std::filesystem::file_time_type modified{};
};

inventory load_inventory(const std::filesystem::path& path);
std::size_t generate_inventory(const std::filesystem::path& inventory, const std::filesystem::path& catalog);
}
