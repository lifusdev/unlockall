#pragma once
#include "lifus/unlockall/mem/Emu.h"

namespace uwu::sem {
struct block {
    std::uint64_t address{};
    std::vector<std::byte> bytes;
};

struct region {
    std::uint64_t address{};
    std::span<const std::byte> bytes;
};

struct branch_set {
    std::vector<std::uint64_t> targets;
    std::size_t cases{};
};

bool step(emulator::state& state, std::span<const std::byte> bytes, std::uint64_t address,
    std::span<const block> reads) noexcept;
bool zero(std::span<const std::byte> bytes, std::uint64_t address, unsigned xmm) noexcept;
std::optional<std::uint64_t> constant(const emulator::decoded& instruction, ZydisRegister output) noexcept;
std::optional<branch_set> targets(std::span<const emulator::decoded> instructions,
    std::span<const region> regions, std::size_t cases = 65536) noexcept;
}
