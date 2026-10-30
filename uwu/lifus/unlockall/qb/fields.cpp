#include "lifus/unlockall/qb/Image.h"
#include "lifus/unlockall/mem/Triton.h"
#include <algorithm>
#include <set>

namespace uwu::sdk::locators {
namespace {
ZydisRegister enclosing(ZydisRegister reg) {
    return ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, reg);
}

std::optional<std::uint64_t> direct(const emulator::decoded& instruction) {
    const auto& operand = instruction.operands[0];
    if (operand.type != ZYDIS_OPERAND_TYPE_IMMEDIATE || !operand.imm.is_relative)
        return {};
    return instruction.address + instruction.instruction.length + operand.imm.value.s;
}

std::optional<std::vector<field_source>> sources(const emulator::program& code, std::size_t end,
    ZydisRegister output, std::span<const code_jump> jumps, const code_image& image, bool partial = false) {
    const auto& instructions = code.instructions;
    std::vector<std::vector<std::size_t>> predecessors(instructions.size());
    for (std::size_t i = 0; i < instructions.size(); ++i) {
        const auto& instruction = instructions[i];
        const auto category = instruction.instruction.meta.category;
        const auto edge = [&](std::uint64_t address) {
            const auto found = std::lower_bound(instructions.begin(), instructions.end(), address,
                [](const auto& value, auto at) { return value.address < at; });
            if (found != instructions.end() && found->address == address)
                predecessors[found - instructions.begin()].push_back(i);
        };
        if (category != ZYDIS_CATEGORY_RET && category != ZYDIS_CATEGORY_UNCOND_BR &&
            category != ZYDIS_CATEGORY_INTERRUPT &&
            !(category == ZYDIS_CATEGORY_CALL && direct(instruction) &&
                image.no_return(*direct(instruction))))
            edge(instruction.address + instruction.instruction.length);
        if (category == ZYDIS_CATEGORY_UNCOND_BR || category == ZYDIS_CATEGORY_COND_BR)
            if (const auto target = direct(instruction))
                edge(*target);
        if (category == ZYDIS_CATEGORY_UNCOND_BR && !direct(instruction)) {
            const auto found = std::find_if(jumps.begin(), jumps.end(),
                [&](const auto& jump) { return jump.instruction == instruction.address; });
            if (found == jumps.end() || found->targets.empty())
                return {};
            for (const auto target : found->targets)
                edge(target);
        }
    }
    std::vector<std::pair<std::size_t, ZydisRegister>> pending;
    for (const auto index : predecessors[end])
        pending.emplace_back(index, enclosing(output));
    std::set<std::pair<std::size_t, ZydisRegister>> seen;
    std::vector<field_source> result;
    while (!pending.empty()) {
        const auto [index, reg] = pending.back();
        pending.pop_back();
        if (!seen.emplace(index, reg).second)
            continue;
        if (seen.size() > 4096)
            return {};
        const auto& instruction = instructions[index];
        if (instruction.instruction.meta.category == ZYDIS_CATEGORY_CALL) {
            if (reg == ZYDIS_REGISTER_RAX) {
                if (!direct(instruction) && !partial)
                    return {};
                result.push_back({instruction.address, direct(instruction).value_or(0), {}});
                continue;
            }
            if (reg == ZYDIS_REGISTER_RCX || reg == ZYDIS_REGISTER_RDX || reg == ZYDIS_REGISTER_R8 ||
                reg == ZYDIS_REGISTER_R9 || reg == ZYDIS_REGISTER_R10 || reg == ZYDIS_REGISTER_R11)
                return {};
        }
        auto input = reg;
        bool terminal{};
        for (unsigned i = 0; i < instruction.instruction.operand_count; ++i) {
            const auto& operand = instruction.operands[i];
            if (operand.type != ZYDIS_OPERAND_TYPE_REGISTER ||
                !(operand.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) || enclosing(operand.reg.value) != reg)
                continue;
            if (!i && instruction.instruction.mnemonic == ZYDIS_MNEMONIC_MOV && operand.size == 64 &&
                instruction.operands[1].type == ZYDIS_OPERAND_TYPE_REGISTER &&
                instruction.operands[1].size == 64)
                input = enclosing(instruction.operands[1].reg.value);
            else {
                auto value = slice(code, index + 1, reg);
                if (!value && sem::constant(instruction, reg))
                    value =
                        expression{{instruction.address, instruction.address + instruction.instruction.length,
                                       {instruction}, true},
                            reg, 0};
                if (!value && !partial)
                    return {};
                result.push_back({instruction.address, 0, value});
                terminal = true;
                break;
            }
        }
        if (terminal)
            continue;
        if (predecessors[index].empty())
            return {};
        for (const auto prior : predecessors[index])
            pending.emplace_back(prior, input);
    }
    if (result.empty())
        return {};
    std::sort(result.begin(), result.end(),
        [](const auto& a, const auto& b) { return a.instruction < b.instruction; });
    result.erase(std::unique(result.begin(), result.end(),
                     [](const auto& a, const auto& b) { return a.instruction == b.instruction; }),
        result.end());
    return result;
}
}

std::optional<std::vector<field_source>> code_image::origins(
    const emulator::program& program, std::size_t before, ZydisRegister output) const {
    if (before >= program.instructions.size() ||
        ZydisRegisterGetClass(enclosing(output)) != ZYDIS_REGCLASS_GPR64)
        return {};
    return sources(program, before, output, jumps(program), *this, true);
}

}
