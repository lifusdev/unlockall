#include "lifus/unlockall/qb/Image.h"
#include <algorithm>
#include <deque>

namespace uwu::sdk::locators {
namespace {
std::uint32_t bit(ZydisRegister reg) {
    if (ZydisRegisterGetClass(reg) != ZYDIS_REGCLASS_GPR64)
        return 0;
    const auto id = ZydisRegisterGetId(reg);
    return id >= 0 && id < 16 ? 1u << id : 0;
}
}

bool code_image::same_value(const emulator::program& program, std::size_t first, ZydisRegister input,
    std::size_t last, ZydisRegister output) const {
    return track(program, first, input, last, output, false);
}

bool code_image::dominates(const emulator::program& program, std::size_t first, std::size_t last) const {
    return track(program, first, ZYDIS_REGISTER_NONE, last, ZYDIS_REGISTER_NONE, true);
}

bool code_image::track(const emulator::program& program, std::size_t first, ZydisRegister input,
    std::size_t last, ZydisRegister output, bool marker, bool any) const {
    const auto& code = program.instructions;
    const auto input_mask = marker ? 1u << 16 : bit(input);
    const auto output_mask = marker ? 1u << 16 : bit(output);
    if (first >= code.size() || last >= code.size() || !input_mask || !output_mask || code.size() > 32768)
        return false;
    const auto start = std::lower_bound(code.begin(), code.end(), program.begin,
        [](const auto& instruction, auto address) { return instruction.address < address; });
    if (start == code.end() || start->address != program.begin)
        return false;
    const auto entry = static_cast<std::size_t>(start - code.begin());
    const auto branches = jumps(program);
    if (std::any_of(
            branches.begin(), branches.end(), [](const auto& branch) { return branch.targets.empty(); }))
        return false;
    std::vector<std::vector<std::size_t>> edges(code.size());
    for (std::size_t i = 0; i < code.size(); ++i) {
        const auto& instruction = code[i];
        const auto category = instruction.instruction.meta.category;
        const auto& argument = instruction.operands[0];
        const bool exits = category == ZYDIS_CATEGORY_CALL && argument.type == ZYDIS_OPERAND_TYPE_IMMEDIATE &&
            argument.imm.is_relative &&
            no_return(instruction.address + instruction.instruction.length + argument.imm.value.s);
        const auto edge = [&](std::uint64_t address) {
            const auto found = std::lower_bound(code.begin(), code.end(), address,
                [](const auto& value, auto at) { return value.address < at; });
            if (found != code.end() && found->address == address)
                edges[i].push_back(found - code.begin());
        };
        if (category != ZYDIS_CATEGORY_RET && category != ZYDIS_CATEGORY_UNCOND_BR &&
            category != ZYDIS_CATEGORY_INTERRUPT && category != ZYDIS_CATEGORY_SYSRET && !exits)
            edge(instruction.address + instruction.instruction.length);
        if (category == ZYDIS_CATEGORY_COND_BR || category == ZYDIS_CATEGORY_UNCOND_BR) {
            const auto& operand = instruction.operands[0];
            if (operand.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && operand.imm.is_relative)
                edge(instruction.address + instruction.instruction.length + operand.imm.value.s);
            else {
                const auto found = std::find_if(branches.begin(), branches.end(),
                    [&](const auto& branch) { return branch.instruction == instruction.address; });
                if (found == branches.end())
                    return false;
                for (auto address : found->targets)
                    edge(address);
            }
        }
    }
    std::vector<std::optional<std::uint32_t>> incoming(code.size());
    std::deque<std::size_t> pending{entry};
    incoming[entry] = 0;
    std::size_t steps{};
    while (!pending.empty()) {
        if (++steps > code.size() * 64)
            return false;
        const auto at = pending.front();
        pending.pop_front();
        auto state = at == first ? input_mask : *incoming[at];
        const auto stack = state & bit(ZYDIS_REGISTER_RSP);
        const auto& instruction = code[at];
        const auto& out = instruction.operands[0];
        const auto& in = instruction.operands[1];
        const bool copy = instruction.instruction.mnemonic == ZYDIS_MNEMONIC_MOV &&
            out.type == ZYDIS_OPERAND_TYPE_REGISTER && out.size == 64 &&
            in.type == ZYDIS_OPERAND_TYPE_REGISTER && in.size == 64 && (state & bit(in.reg.value));
        const bool address = instruction.instruction.mnemonic == ZYDIS_MNEMONIC_LEA &&
            out.type == ZYDIS_OPERAND_TYPE_REGISTER && out.size == 64 &&
            in.type == ZYDIS_OPERAND_TYPE_MEMORY && in.mem.index == ZYDIS_REGISTER_NONE &&
            !in.mem.disp.value && (state & bit(in.mem.base));
        for (unsigned i = 0; i < instruction.instruction.operand_count; ++i) {
            const auto& operand = instruction.operands[i];
            if (operand.type == ZYDIS_OPERAND_TYPE_REGISTER &&
                (operand.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE))
                state &=
                    ~bit(ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, operand.reg.value));
        }
        if (copy || address)
            state |= bit(out.reg.value);
        if (instruction.instruction.meta.category == ZYDIS_CATEGORY_CALL) {
            state &= ~(bit(ZYDIS_REGISTER_RAX) | bit(ZYDIS_REGISTER_RCX) | bit(ZYDIS_REGISTER_RDX) |
                bit(ZYDIS_REGISTER_R8) | bit(ZYDIS_REGISTER_R9) | bit(ZYDIS_REGISTER_R10) |
                bit(ZYDIS_REGISTER_R11));
            state |= stack;
        }
        for (const auto next : edges[at]) {
            const auto merged =
                incoming[next] ? (any ? *incoming[next] | state : *incoming[next] & state) : state;
            if (!incoming[next] || merged != *incoming[next]) {
                incoming[next] = merged;
                pending.push_back(next);
            }
        }
    }
    if (first == last)
        return incoming[last] && (marker || input == output);
    return incoming[last] && ((*incoming[last] & output_mask) != 0);
}
}
