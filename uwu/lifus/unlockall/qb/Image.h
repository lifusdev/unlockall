#pragma once
#include "lifus/unlockall/qb/Scan.h"
#include <string_view>
#include <set>
#include <map>

namespace uwu::sdk::locators {
struct code_reference {
    std::uint64_t target{}, instruction{};
    function_record function{};
    ZydisRegister output = ZYDIS_REGISTER_NONE;
};

enum class code_status { complete, invalid, unsupported, budget, cancelled };

struct code_references {
    code_status status = code_status::complete;
    std::vector<code_reference> values;
    std::size_t rejected_functions{};
};

struct code_limits {
    std::size_t instructions = 32768;
    std::size_t functions = 128;
    std::size_t function_bytes = 65536;
    std::size_t leaf_bytes = 1024;
};

struct field_source {
    std::uint64_t instruction{}, target{};
    std::optional<expression> value;
};

struct code_jump {
    std::uint64_t begin{}, instruction{};
    std::vector<std::uint64_t> targets;
    std::size_t cases{};
};

class code_image {
  public:
    code_image(std::span<const std::byte> code, std::uint32_t code_rva, std::uint64_t base,
        std::span<const function_record> functions, std::span<const std::byte> data, std::uint32_t data_rva);
    [[nodiscard]] code_references references(
        std::string_view literal, std::stop_token stop = {}, std::size_t limit = 256) const;
    [[nodiscard]] std::optional<emulator::program> body(
        std::uint64_t entry, const code_limits& limits = {}) const;
    [[nodiscard]] std::optional<function_record> function(std::uint64_t address) const;
    [[nodiscard]] std::vector<code_jump> jumps(const emulator::program& program) const;
    [[nodiscard]] std::optional<std::vector<field_source>> origins(
        const emulator::program& program, std::size_t before, ZydisRegister output) const;
    [[nodiscard]] bool same_value(const emulator::program& program, std::size_t first, ZydisRegister input,
        std::size_t last, ZydisRegister output) const;
    [[nodiscard]] bool no_return(std::uint64_t entry) const;
    [[nodiscard]] bool dominates(const emulator::program& program, std::size_t first, std::size_t last) const;

    [[nodiscard]] std::uint64_t base() const noexcept { return base_; }

    [[nodiscard]] std::span<const std::byte> bytes(std::uint64_t address, std::size_t size) const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> value(const expression& expression) const;

  private:
    [[nodiscard]] bool terminal_fault(const function_record& record) const;
    [[nodiscard]] bool track(const emulator::program& program, std::size_t first, ZydisRegister input,
        std::size_t last, ZydisRegister output, bool marker, bool any = false) const;

    struct branch_cache {
        std::vector<emulator::decoded> instructions;
        std::optional<code_jump> result;
    };

    [[nodiscard]] std::optional<code_jump> resolve(std::span<const emulator::decoded> block) const;
    mutable std::vector<branch_cache> branches_;
    mutable std::map<std::uint64_t, bool> exits_;
    std::set<std::uint64_t> entries_;
    std::span<const std::byte> code_, data_;
    std::span<const function_record> functions_;
    std::uint64_t base_{};
    std::uint32_t code_rva_{}, data_rva_{};
};
}
