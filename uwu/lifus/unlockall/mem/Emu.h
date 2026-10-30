#pragma once
#include "lifus/unlockall/mem/Memory.h"
#include <Zydis/Zydis.h>
#include <array>
#include <bitset>
#include <optional>
#include <span>
#include <vector>

namespace uwu {
class shadow_memory;
enum class emulation_status {
    complete,
    unsupported,
    invalid_operand,
    read_failed,
    write_blocked,
    control_flow,
    budget_exhausted
};

struct emulation_result {
    emulation_status status{};
    std::uint64_t address{};
    ZydisMnemonic mnemonic{};
    std::size_t steps{};

    [[nodiscard]] bool complete() const noexcept { return status == emulation_status::complete; }
};

class emulator {
  public:
    struct access {
        std::uint64_t instruction{}, address{}, value{};
        unsigned bits{};
        DWORD error{};
        bool supplied{}, stack{};
    };

    struct observer {
        void* context{};
        void (*read)(void*, const access&) noexcept {};
    };

    struct state {
        std::array<std::uint64_t, 16> registers{};
        std::array<std::array<std::uint64_t, 2>, 16> vectors{};
        std::uint64_t gs{}, flags{};
    };

    struct decoded {
        std::uint64_t address{};
        std::array<std::byte, 15> bytes{};
        ZydisDecodedInstruction instruction{};
        std::array<ZydisDecodedOperand, ZYDIS_MAX_OPERAND_COUNT> operands{};
    };

    struct program {
        std::uint64_t begin{}, end{};
        std::vector<decoded> instructions;
        bool linear{};
        bool sparse{};
    };

    explicit emulator(const memory& process) : process_(process) {}

    [[nodiscard]] static std::optional<program> decode(
        std::span<const std::byte> bytes_value, std::uint64_t address_value, bool prefix = false);
    void reset(std::uint64_t teb = 0) noexcept;
    void set(ZydisRegister reg, std::uint64_t value_data);
    [[nodiscard]] std::uint64_t get(ZydisRegister reg) const;
    void stack(std::uint32_t offset, std::uint64_t value_data);
    [[nodiscard]] std::uint64_t stack(std::uint32_t offset) const;
    void input(std::uint64_t address_value, std::span<const std::byte> bytes_value);
    void shadow(shadow_memory& memory);
    [[nodiscard]] emulation_result run(const program& code, std::size_t budget = 2048);

    void observe(observer observer_value) noexcept { observer_ = observer_value; }

    [[nodiscard]] state snapshot() const noexcept { return {registers_, vectors_, gs_, flags_}; }

    [[nodiscard]] std::span<const std::byte> stack_data() const noexcept { return stack_; }

  private:
    struct region {
        std::uint64_t address;
        std::span<const std::byte> bytes;
    };

    static constexpr std::uint64_t stack_base = 0x70000000000ull;
    const memory& process_;
    std::array<std::uint64_t, 16> registers_{};
    std::array<std::array<std::uint64_t, 2>, 16> vectors_{};
    std::array<std::byte, 0x10000> stack_{};
    std::bitset<0x10000> initialized_;
    shadow_memory* shadow_{};
    std::vector<region> inputs_;
    std::uint64_t ip_{}, next_{}, gs_{}, flags_{2};
    std::uint64_t current_{};
    unsigned address_width_{64};
    observer observer_{};
    [[nodiscard]] std::uint64_t address(const ZydisDecodedOperand& operand, bool segment = true) const;
    [[nodiscard]] std::uint64_t load(std::uint64_t address_value, unsigned bits_value) const;
    void store(std::uint64_t address_value, unsigned bits_value, std::uint64_t value_data);
    [[nodiscard]] std::uint64_t value(const ZydisDecodedOperand& operand) const;
    void write(const ZydisDecodedOperand& operand, std::uint64_t value_data);
    void arithmetic(
        std::uint64_t a, std::uint64_t b, std::uint64_t result_value, unsigned bits_value, bool subtract);
    void logical(std::uint64_t result_value, unsigned bits_value);
    [[nodiscard]] std::optional<bool> condition(ZydisMnemonic mnemonic) const;
    void step(const decoded& instruction_value);
    bool triton_step(const decoded& instruction);
};
}
