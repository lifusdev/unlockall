#pragma once
#include <filesystem>

namespace uwu {
struct paths {
    static std::filesystem::path executable();
    static std::filesystem::path data_dir();
    static std::filesystem::path inventory();
};
}
