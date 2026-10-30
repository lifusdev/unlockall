#pragma once
#include "lifus/unlockall/mem/Emu.h"
#include <vector>

namespace uwu::sdk::locators {
struct fault_witness {
    std::uint64_t block{}, instruction{}, address{};
    unsigned bytes{};
    std::vector<std::uint64_t> prefix;
};

struct fault_analysis {
    std::vector<fault_witness> faults;
    unsigned blocks{}, unknown_memory{}, unsupported{};
    bool opcode_binding{};
};

fault_analysis normal_execution_faults(const uwu::emulator::program& body, std::uint64_t user_min = 0x10000,
    std::uint64_t user_end = 0x800000000000ull);
}
