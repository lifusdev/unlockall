#include "lifus/unlockall/qb/Registry.h"
#include <algorithm>
#include <set>

namespace uwu::sdk::locators {
namespace {
bool reg(const ZydisDecodedOperand& operand) {
    return operand.type == ZYDIS_OPERAND_TYPE_REGISTER && operand.size == 64 &&
        ZydisRegisterGetClass(operand.reg.value) == ZYDIS_REGCLASS_GPR64;
}

bool member(const ZydisDecodedOperand& operand, ZydisRegister owner) {
    return operand.type == ZYDIS_OPERAND_TYPE_MEMORY && operand.size == 64 && operand.mem.base == owner &&
        operand.mem.index == ZYDIS_REGISTER_NONE && operand.mem.segment != ZYDIS_REGISTER_FS &&
        operand.mem.segment != ZYDIS_REGISTER_GS && operand.mem.disp.value >= 0 &&
        operand.mem.disp.value <= 0x100000;
}

std::optional<std::uint64_t> destination(const emulator::decoded& instruction) {
    const auto category = instruction.instruction.meta.category;
    const auto& operand = instruction.operands[0];
    if ((category != ZYDIS_CATEGORY_COND_BR && category != ZYDIS_CATEGORY_UNCOND_BR) ||
        operand.type != ZYDIS_OPERAND_TYPE_IMMEDIATE || !operand.imm.is_relative)
        return {};
    return instruction.address + instruction.instruction.length + operand.imm.value.s;
}

std::optional<std::uint32_t> first_field(
    const emulator::program& program, std::size_t start, ZydisRegister owner, ZydisRegister end) {
    std::set<ZydisRegister> aliases{end};
    std::optional<std::uint32_t> result;
    auto expected = program.instructions[start].address;
    for (std::size_t i = start; i < program.instructions.size(); ++i) {
        const auto& instruction = program.instructions[i];
        const auto category = instruction.instruction.meta.category;
        if (instruction.address != expected || category == ZYDIS_CATEGORY_CALL ||
            category == ZYDIS_CATEGORY_RET || category == ZYDIS_CATEGORY_COND_BR ||
            category == ZYDIS_CATEGORY_UNCOND_BR || category == ZYDIS_CATEGORY_INTERRUPT)
            break;
        expected += instruction.instruction.length;
        const auto& out = instruction.operands[0];
        const auto& in = instruction.operands[1];
        if (instruction.instruction.mnemonic == ZYDIS_MNEMONIC_SUB && reg(out) &&
            aliases.contains(out.reg.value) && member(in, owner)) {
            const auto field = static_cast<std::uint32_t>(in.mem.disp.value);
            if (result && *result != field)
                return {};
            result = field;
        }
        const bool copy = instruction.instruction.mnemonic == ZYDIS_MNEMONIC_MOV && reg(out) && reg(in) &&
            aliases.contains(in.reg.value);
        for (unsigned j = 0; j < instruction.instruction.operand_count; ++j) {
            const auto& operand = instruction.operands[j];
            if (operand.type != ZYDIS_OPERAND_TYPE_REGISTER ||
                !(operand.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE))
                continue;
            const auto written =
                ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, operand.reg.value);
            if (written == owner)
                return result;
            aliases.erase(written);
        }
        if (copy)
            aliases.insert(out.reg.value);
    }
    return result;
}
}

std::vector<registry_descriptor> registries(const emulator::program& program) {
    const auto& code = program.instructions;
    std::vector<registry_descriptor> result;
    if (std::any_of(code.begin(), code.end(), [](const auto& instruction) {
            return instruction.instruction.meta.category == ZYDIS_CATEGORY_UNCOND_BR &&
                !destination(instruction);
        }))
        return result;
    for (std::size_t i = 1; i + 2 < code.size(); ++i) {
        const auto& load = code[i - 1];
        const auto& compare = code[i];
        const auto& branch = code[i + 1];
        if (load.address + load.instruction.length != compare.address ||
            compare.address + compare.instruction.length != branch.address ||
            compare.instruction.mnemonic != ZYDIS_MNEMONIC_CMP ||
            branch.instruction.mnemonic != ZYDIS_MNEMONIC_JNZ || !reg(compare.operands[0]) ||
            compare.operands[1].type != ZYDIS_OPERAND_TYPE_MEMORY ||
            load.instruction.mnemonic != ZYDIS_MNEMONIC_MOV || !reg(load.operands[0]) ||
            load.operands[0].reg.value != compare.operands[0].reg.value)
            continue;
        const auto owner = compare.operands[1].mem.base;
        if (owner == compare.operands[0].reg.value || ZydisRegisterGetClass(owner) != ZYDIS_REGCLASS_GPR64 ||
            !member(compare.operands[1], owner) || !member(load.operands[1], owner))
            continue;
        const auto target = destination(branch);
        if (!target)
            continue;
        const auto found = std::lower_bound(code.begin(), code.end(), *target,
            [](const auto& instruction, auto at) { return instruction.address < at; });
        if (found == code.end() || found->address != *target || code.end() - found < 2)
            continue;
        const auto& store = *found;
        const auto& advance = *(found + 1);
        if (store.instruction.mnemonic != ZYDIS_MNEMONIC_MOV ||
            !member(store.operands[0], compare.operands[0].reg.value) || store.operands[0].mem.disp.value ||
            !reg(store.operands[1]) || store.address + store.instruction.length != advance.address ||
            advance.instruction.mnemonic != ZYDIS_MNEMONIC_ADD || !member(advance.operands[0], owner) ||
            advance.operands[0].mem.disp.value != load.operands[1].mem.disp.value ||
            advance.operands[1].type != ZYDIS_OPERAND_TYPE_IMMEDIATE || advance.operands[1].imm.value.u != 8)
            continue;
        if (std::any_of(code.begin(), code.end(), [&](const auto& instruction) {
                if (instruction.address == branch.address)
                    return false;
                const auto to = destination(instruction);
                return to && *to >= store.address && *to <= advance.address;
            }))
            continue;
        const auto first = first_field(program, i + 2, owner, compare.operands[0].reg.value);
        const auto owner_value = slice(program, i, owner);
        if (!first || !owner_value || owner_value->inputs || *first == load.operands[1].mem.disp.value ||
            *first == compare.operands[1].mem.disp.value ||
            load.operands[1].mem.disp.value == compare.operands[1].mem.disp.value)
            continue;
        result.push_back({*owner_value, compare.address, store.address, *first,
            static_cast<std::uint32_t>(load.operands[1].mem.disp.value),
            static_cast<std::uint32_t>(compare.operands[1].mem.disp.value), 8});
    }
    return result;
}
}
