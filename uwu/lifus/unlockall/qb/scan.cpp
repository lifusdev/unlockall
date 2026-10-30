#include "lifus/unlockall/qb/Scan.h"
#include <algorithm>
#include <map>

namespace uwu::sdk::locators {
namespace {
using resolved_branches = std::map<std::uint64_t, std::vector<std::uint64_t>>;

struct location {
    ZydisRegister reg = ZYDIS_REGISTER_NONE;
    std::int64_t slot{};
    bool stack{};
};

std::uint32_t dependency(ZydisRegister reg) {
    if (reg == ZYDIS_REGISTER_RFLAGS || reg == ZYDIS_REGISTER_EFLAGS)
        return 0;
    const auto enclosing = ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, reg);
    if (ZydisRegisterGetClass(enclosing) != ZYDIS_REGCLASS_GPR64)
        return 0;
    const auto id = ZydisRegisterGetId(enclosing);
    return id >= 0 && id < 16 ? 1u << id : 0;
}

bool local_slot(const ZydisDecodedOperand& o) {
    return o.type == ZYDIS_OPERAND_TYPE_MEMORY && o.mem.base == ZYDIS_REGISTER_RSP &&
        o.mem.index == ZYDIS_REGISTER_NONE;
}

std::uint32_t tracked_dependency(ZydisRegister reg) {
    if (ZydisRegisterGetClass(reg) == ZYDIS_REGCLASS_XMM) {
        const auto id = ZydisRegisterGetId(reg);
        return id >= 0 && id < 16 ? 1u << (16 + id) : 0;
    }
    return dependency(reg);
}

}

static std::optional<expression> trace(const emulator::program& code, std::size_t before, location output,
    std::optional<location> input_value = std::nullopt, const resolved_branches* branches = nullptr,
    bool* return_cancellation = nullptr) {
    if (return_cancellation)
        *return_cancellation = false;
    if (before > code.instructions.size() || (!output.stack && !tracked_dependency(output.reg)))
        return std::nullopt;
    expression result_value{{0, 0, {}, true}, output.reg, 0};
    auto needed = output.stack ? 0 : tracked_dependency(output.reg);
    std::uint32_t bound{};
    ZydisAccessedFlagsMask flags{};
    std::map<std::int64_t, unsigned> slots;
    if (output.stack)
        slots.emplace(output.slot, 64);
    std::vector<std::size_t> definitions;
    std::vector<std::vector<std::size_t>> predecessors(before + 1);
    const auto boundary = code.instructions.begin() + std::min(before + 1, code.instructions.size());
    for (std::size_t at = 0; at < before; ++at) {
        const auto& d = code.instructions[at];
        const auto category = d.instruction.meta.category;
        if (category != ZYDIS_CATEGORY_RET && category != ZYDIS_CATEGORY_UNCOND_BR)
            predecessors[at + 1].push_back(at);
        if ((category == ZYDIS_CATEGORY_COND_BR || category == ZYDIS_CATEGORY_UNCOND_BR) &&
            d.operands[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE && d.operands[0].imm.is_relative) {
            const auto address_value = d.address + d.instruction.length + d.operands[0].imm.value.s;
            const auto target_value = std::lower_bound(code.instructions.begin(), boundary, address_value,
                [](const auto& value_data, std::uint64_t at) { return value_data.address < at; });
            if (target_value != boundary && target_value->address == address_value)
                predecessors[target_value - code.instructions.begin()].push_back(at);
        } else if (category == ZYDIS_CATEGORY_UNCOND_BR && branches) {
            if (const auto found = branches->find(d.address); found != branches->end())
                for (const auto address_value : found->second) {
                    const auto target_value =
                        std::lower_bound(code.instructions.begin(), boundary, address_value,
                            [](const auto& value_data, std::uint64_t at) { return value_data.address < at; });
                    if (target_value != boundary && target_value->address == address_value)
                        predecessors[target_value - code.instructions.begin()].push_back(at);
                }
        }
    }
    std::vector<bool> reaches(before + 1);
    std::vector<std::size_t> ancestors{before};
    while (!ancestors.empty()) {
        const auto at = ancestors.back();
        ancestors.pop_back();
        if (reaches[at])
            continue;
        reaches[at] = true;
        ancestors.insert(ancestors.end(), predecessors[at].begin(), predecessors[at].end());
    }
    for (std::size_t index = before; index > 0; --index) {
        if (!reaches[index - 1])
            continue;
        needed &= ~(1u << 4);
        if (input_value) {
            if (input_value->stack)
                slots.erase(input_value->slot);
            else {
                bound |= needed & tracked_dependency(input_value->reg);
                needed &= ~tracked_dependency(input_value->reg);
            }
        }
        const auto& d = code.instructions[index - 1];
        const auto& i = d.instruction;
        if (i.mnemonic == ZYDIS_MNEMONIC_CALL && ((needed & 0x3f0f07u) || flags)) {
            if (return_cancellation)
                *return_cancellation =
                    (needed & 0x3f0f07u) == dependency(ZYDIS_REGISTER_RAX) && !flags && slots.empty();
            return std::nullopt;
        }
        const auto writes = i.cpu_flags
            ? (i.cpu_flags->modified | i.cpu_flags->set_0 | i.cpu_flags->set_1 | i.cpu_flags->undefined)
            : 0;
        bool selected = (flags & writes) != 0;
        for (unsigned j = 0; j < i.operand_count; ++j) {
            const auto& o = d.operands[j];
            if (!(o.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE))
                continue;
            if (o.type == ZYDIS_OPERAND_TYPE_REGISTER && (needed & tracked_dependency(o.reg.value)))
                selected = true;
            if (local_slot(o) && slots.contains(o.mem.disp.value))
                selected = true;
        }
        if (!selected)
            continue;
        if (i.meta.category == ZYDIS_CATEGORY_CALL || i.meta.category == ZYDIS_CATEGORY_RET ||
            i.meta.category == ZYDIS_CATEGORY_COND_BR || i.meta.category == ZYDIS_CATEGORY_UNCOND_BR ||
            i.mnemonic == ZYDIS_MNEMONIC_PUSH || i.mnemonic == ZYDIS_MNEMONIC_POP)
            return std::nullopt;
        if (i.cpu_flags) {
            if (flags & i.cpu_flags->undefined)
                return std::nullopt;
            flags = (flags & ~writes) | i.cpu_flags->tested;
        }
        for (unsigned j = 0; j < i.operand_count; ++j) {
            const auto& o = d.operands[j];
            if (!(o.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE))
                continue;
            if (o.type == ZYDIS_OPERAND_TYPE_REGISTER && !(o.actions & ZYDIS_OPERAND_ACTION_CONDWRITE)) {
                const bool vector_value = ZydisRegisterGetClass(o.reg.value) == ZYDIS_REGCLASS_XMM;
                const bool complete_value = vector_value ? o.size == 128 ||
                        i.mnemonic == ZYDIS_MNEMONIC_MOVQ || i.mnemonic == ZYDIS_MNEMONIC_MOVD ||
                        (i.mnemonic == ZYDIS_MNEMONIC_MOVSS &&
                            d.operands[1].type == ZYDIS_OPERAND_TYPE_MEMORY)
                                                         : o.size >= 32;
                if (complete_value)
                    needed &= ~tracked_dependency(o.reg.value);
            }
            if (o.type == ZYDIS_OPERAND_TYPE_MEMORY) {
                if (!local_slot(o))
                    return std::nullopt;
                const auto found = slots.find(o.mem.disp.value);
                if (found != slots.end()) {
                    if (o.size < found->second)
                        return std::nullopt;
                    slots.erase(found);
                }
            }
        }
        bool zero = false;
        if ((i.mnemonic == ZYDIS_MNEMONIC_XOR || i.mnemonic == ZYDIS_MNEMONIC_XORPS) &&
            i.operand_count_visible == 2 && d.operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
            d.operands[1].type == ZYDIS_OPERAND_TYPE_REGISTER &&
            d.operands[0].reg.value == d.operands[1].reg.value)
            zero = true;
        for (unsigned j = 0; j < i.operand_count; ++j) {
            const auto& o = d.operands[j];
            if (o.type == ZYDIS_OPERAND_TYPE_MEMORY) {
                needed |= tracked_dependency(o.mem.base) | tracked_dependency(o.mem.index);
                if (local_slot(o) && (o.actions & ZYDIS_OPERAND_ACTION_MASK_READ) &&
                    i.mnemonic != ZYDIS_MNEMONIC_LEA)
                    slots[o.mem.disp.value] =
                        std::max(slots[o.mem.disp.value], static_cast<unsigned>(o.size));
            }
            if (o.type == ZYDIS_OPERAND_TYPE_REGISTER && (o.actions & ZYDIS_OPERAND_ACTION_MASK_READ) &&
                !zero)
                needed |= tracked_dependency(o.reg.value);
        }
        result_value.code.instructions.push_back(d);
        definitions.push_back(index - 1);
    }
    needed &= ~(1u << 4);
    if (input_value) {
        if (input_value->stack)
            slots.erase(input_value->slot);
        else {
            bound |= needed & tracked_dependency(input_value->reg);
            needed &= ~tracked_dependency(input_value->reg);
        }
    }
    if (flags || !slots.empty() || result_value.code.instructions.empty())
        return std::nullopt;
    for (const auto definition_value : definitions) {
        std::vector<bool> seen(before + 1);
        std::vector<std::size_t> pending{0};
        while (!pending.empty()) {
            const auto at = pending.back();
            pending.pop_back();
            if (at == definition_value || at > before || seen[at])
                continue;
            if (at == before)
                return std::nullopt;
            seen[at] = true;
            const auto& d = code.instructions[at];
            const auto category = d.instruction.meta.category;
            if (category == ZYDIS_CATEGORY_RET)
                continue;
            if (category != ZYDIS_CATEGORY_UNCOND_BR)
                pending.push_back(at + 1);
            if (category == ZYDIS_CATEGORY_UNCOND_BR || category == ZYDIS_CATEGORY_COND_BR) {
                const auto& o = d.operands[0];
                if (o.type != ZYDIS_OPERAND_TYPE_IMMEDIATE || !o.imm.is_relative) {
                    if (!branches)
                        return std::nullopt;
                    const auto found = branches->find(d.address);
                    if (found == branches->end())
                        return std::nullopt;
                    for (const auto address_value : found->second) {
                        const auto target_value = std::lower_bound(code.instructions.begin(),
                            code.instructions.end(), address_value,
                            [](const auto& value_data, std::uint64_t at) { return value_data.address < at; });
                        if (target_value == code.instructions.end() || target_value->address != address_value)
                            return std::nullopt;
                        pending.push_back(static_cast<std::size_t>(target_value - code.instructions.begin()));
                    }
                    continue;
                }
                const auto target_value = d.address + d.instruction.length + o.imm.value.s;
                const auto found =
                    std::lower_bound(code.instructions.begin(), code.instructions.end(), target_value,
                        [](const auto& v, std::uint64_t address_value) { return v.address < address_value; });
                if (found != code.instructions.end() && found->address == target_value)
                    pending.push_back(static_cast<std::size_t>(found - code.instructions.begin()));
            }
        }
    }
    std::reverse(result_value.code.instructions.begin(), result_value.code.instructions.end());
    result_value.code.begin = result_value.code.instructions.front().address;
    const auto& last = result_value.code.instructions.back();
    result_value.code.end = last.address + last.instruction.length;
    result_value.inputs = needed | bound;
    return result_value;
}

std::optional<expression> slice(const emulator::program& code, std::size_t before, ZydisRegister output) {
    return trace(code, before, {output});
}

}
