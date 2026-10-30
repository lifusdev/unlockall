#pragma once
#include "lifus/unlockall/mem/Emu.h"
#include <functional>
#include <stop_token>
#include <string_view>

namespace uwu::sdk::locators {
struct expression {
    emulator::program code;
    ZydisRegister output = ZYDIS_REGISTER_NONE;
    std::uint32_t inputs{};
};

struct function_record {
    std::uint32_t begin{}, end{}, unwind{};
};

using validation_observer = std::function<void(std::string_view, std::uint64_t, std::uint64_t)>;
[[nodiscard]] std::optional<expression> slice(
    const emulator::program& code, std::size_t before, ZydisRegister output);
}
