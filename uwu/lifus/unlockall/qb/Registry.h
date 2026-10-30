#pragma once
#include "lifus/unlockall/qb/Scan.h"

namespace uwu::sdk::locators {
struct registry_descriptor {
    expression owner;
    std::uint64_t guard{}, append{};
    std::uint32_t first{}, last{}, capacity{};
    std::uint8_t stride{};
};

[[nodiscard]] std::vector<registry_descriptor> registries(const emulator::program& code);
}
