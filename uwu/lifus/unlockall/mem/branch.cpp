#include "lifus/unlockall/mem/Triton.h"
#include <triton/context.hpp>
#include <triton/instruction.hpp>
#include <map>
#include <set>
#include <cstring>

namespace uwu::sem {
namespace {
using variable = triton::engines::symbolic::SharedSymbolicVariable;
using node = triton::ast::SharedAbstractNode;

std::optional<std::map<std::size_t, variable>> variables(const node& root) {
    std::map<std::size_t, variable> result;
    std::set<const triton::ast::AbstractNode*> seen;
    std::vector<node> pending{root};
    while (!pending.empty()) {
        auto value = pending.back();
        pending.pop_back();
        if (!value || !seen.insert(value.get()).second)
            continue;
        if (seen.size() > 16384)
            return {};
        if (value->getType() == triton::ast::VARIABLE_NODE) {
            const auto variable = static_cast<triton::ast::VariableNode*>(value.get())->getSymbolicVariable();
            result.emplace(variable->getId(), variable);
        } else if (value->getType() == triton::ast::REFERENCE_NODE)
            pending.push_back(
                static_cast<triton::ast::ReferenceNode*>(value.get())->getSymbolicExpression()->getAst());
        else
            pending.insert(pending.end(), value->getChildren().begin(), value->getChildren().end());
    }
    return result;
}

std::optional<std::uint64_t> read(std::span<const region> regions, std::uint64_t address, std::size_t size) {
    if (!size || size > 8)
        return {};
    for (const auto& region : regions)
        if (address >= region.address && address - region.address <= region.bytes.size() &&
            size <= region.bytes.size() - static_cast<std::size_t>(address - region.address)) {
            std::uint64_t value{};
            std::memcpy(&value, region.bytes.data() + (address - region.address), size);
            return value;
        }
    return {};
}
}

std::optional<branch_set> targets(std::span<const emulator::decoded> instructions,
    std::span<const region> regions, std::size_t case_limit) noexcept {
    try {
        if (instructions.empty() || instructions.size() > 128 || !case_limit || case_limit > 65536)
            return {};
        const auto& jump = instructions.back();
        if (jump.instruction.mnemonic != ZYDIS_MNEMONIC_JMP ||
            jump.operands[0].type != ZYDIS_OPERAND_TYPE_REGISTER || jump.operands[0].size != 64)
            return {};
        ZydisDecoder decoder{};
        ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
        auto expected = instructions.front().address;
        for (const auto& decoded : instructions) {
            ZydisDecodedInstruction native{};
            std::array<ZydisDecodedOperand, ZYDIS_MAX_OPERAND_COUNT> operands{};
            if (decoded.address != expected || !decoded.instruction.length ||
                decoded.instruction.length > decoded.bytes.size() ||
                !ZYAN_SUCCESS(ZydisDecoderDecodeFull(
                    &decoder, decoded.bytes.data(), decoded.instruction.length, &native, operands.data())) ||
                std::memcmp(&native, &decoded.instruction, sizeof(native)) ||
                std::memcmp(operands.data(), decoded.operands.data(), sizeof(operands)))
                return {};
            expected += decoded.instruction.length;
            for (unsigned i = 0; i < native.operand_count; ++i) {
                const auto type = operands[i].element_type;
                if (type == ZYDIS_ELEMENT_TYPE_FLOAT16 || type == ZYDIS_ELEMENT_TYPE_FLOAT32 ||
                    type == ZYDIS_ELEMENT_TYPE_FLOAT64 || type == ZYDIS_ELEMENT_TYPE_FLOAT80 ||
                    type == ZYDIS_ELEMENT_TYPE_LONGBCD)
                    return {};
            }
        }
        triton::Context ctx(triton::arch::ARCH_X86_64);
        ctx.setMode(triton::modes::AST_OPTIMIZATIONS, true);
        std::set<std::size_t> incoming;
        for (const char* name : {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10",
                 "r11", "r12", "r13", "r14", "r15", "cf", "pf", "af", "zf", "sf", "of", "df"})
            incoming.insert(ctx.symbolizeRegister(ctx.getRegister(name))->getId());
        for (unsigned i = 0; i < 16; ++i)
            incoming.insert(ctx.symbolizeRegister(ctx.getRegister("xmm" + std::to_string(i)))->getId());

        struct input {
            variable value;
            node address;
            std::size_t size{};
            bool local{};
        };

        std::map<std::size_t, input> inputs;
        for (const auto& decoded : instructions.first(instructions.size() - 1)) {
            const auto category = decoded.instruction.meta.category;
            if (category == ZYDIS_CATEGORY_CALL || category == ZYDIS_CATEGORY_RET ||
                category == ZYDIS_CATEGORY_COND_BR || category == ZYDIS_CATEGORY_UNCOND_BR ||
                category == ZYDIS_CATEGORY_INTERRUPT ||
                (decoded.instruction.cpu_flags && (decoded.instruction.cpu_flags->tested & ~0xcd5u)))
                return {};
            ctx.setConcreteRegisterValue(ctx.getRegister("rip"), decoded.address);
            std::optional<std::pair<variable, triton::arch::MemoryAccess>> memory;
            bool segmented{};
            for (unsigned i = 0; i < decoded.instruction.operand_count; ++i) {
                const auto& operand = decoded.operands[i];
                if (operand.type != ZYDIS_OPERAND_TYPE_MEMORY || operand.mem.type == ZYDIS_MEMOP_TYPE_AGEN)
                    continue;
                if (memory || (operand.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) ||
                    !(operand.actions & ZYDIS_OPERAND_ACTION_MASK_READ) || !operand.size ||
                    operand.size > 64 || operand.size % 8)
                    return {};
                const auto reg = [&](ZydisRegister value) -> std::uint64_t {
                    if (value == ZYDIS_REGISTER_NONE)
                        return 0;
                    if (value == ZYDIS_REGISTER_RIP || value == ZYDIS_REGISTER_EIP)
                        return decoded.address + decoded.instruction.length;
                    return static_cast<std::uint64_t>(
                        ctx.getConcreteRegisterValue(ctx.getRegister(ZydisRegisterGetString(value))));
                };
                auto address = reg(operand.mem.base) + reg(operand.mem.index) * operand.mem.scale +
                    operand.mem.disp.value;
                if (decoded.instruction.address_width == 32)
                    address &= 0xffffffffu;
                else if (decoded.instruction.address_width != 64)
                    return {};
                triton::arch::MemoryAccess access(address, operand.size / 8);
                ctx.setConcreteMemoryValue(access, 0);
                memory = std::pair{ctx.symbolizeMemory(access), access};
                segmented =
                    operand.mem.segment == ZYDIS_REGISTER_FS || operand.mem.segment == ZYDIS_REGISTER_GS;
            }
            triton::arch::Instruction instruction;
            instruction.setAddress(decoded.address);
            instruction.setOpcode(decoded.bytes.data(), decoded.instruction.length);
            if (ctx.processing(instruction) != triton::arch::NO_FAULT ||
                !instruction.getStoreAccess().empty())
                return {};
            std::set<ZydisRegister> folded;
            for (unsigned i = 0; i < decoded.instruction.operand_count; ++i) {
                const auto& operand = decoded.operands[i];
                if (operand.type != ZYDIS_OPERAND_TYPE_REGISTER ||
                    !(operand.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE))
                    continue;
                const auto reg =
                    ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, operand.reg.value);
                if (ZydisRegisterGetClass(reg) != ZYDIS_REGCLASS_GPR64 || !folded.insert(reg).second)
                    continue;
                if (const auto value = constant(decoded, reg)) {
                    const auto& target = ctx.getRegister(ZydisRegisterGetString(reg));
                    ctx.concretizeRegister(target);
                    ctx.setConcreteRegisterValue(target, *value);
                }
            }
            if (!memory) {
                if (!instruction.getLoadAccess().empty())
                    return {};
                continue;
            }
            if (instruction.getLoadAccess().size() != 1)
                return {};
            const auto& access = instruction.getLoadAccess().begin()->first;
            if (access.getAddress() != memory->second.getAddress() ||
                access.getSize() != memory->second.getSize())
                return {};
            const auto address = access.getLeaAst();
            const auto dependencies = variables(address);
            if (!address || !dependencies)
                return {};
            bool local = !segmented;
            for (const auto& [id, value] : *dependencies) {
                static_cast<void>(value);
                local &= !incoming.contains(id);
            }
            inputs.emplace(memory->first->getId(), input{memory->first, address, access.getSize(), local});
        }
        const auto expression =
            ctx.getSymbolicRegister(ctx.getRegister(ZydisRegisterGetString(jump.operands[0].reg.value)));
        if (!expression)
            return {};
        const auto goal = expression->getAst();
        std::map<std::size_t, variable> free;
        std::set<std::size_t> derived;
        std::vector<node> pending{goal};
        while (!pending.empty()) {
            const auto dependencies = variables(pending.back());
            pending.pop_back();
            if (!dependencies)
                return {};
            for (const auto& [id, value] : *dependencies) {
                const auto found = inputs.find(id);
                if (found != inputs.end() && found->second.local) {
                    if (derived.insert(id).second)
                        pending.push_back(found->second.address);
                } else
                    free.emplace(id, value);
            }
        }
        unsigned bits{};
        for (const auto& [id, value] : free) {
            static_cast<void>(id);
            if (value->getSize() > 16 || bits + value->getSize() > 16)
                return {};
            bits += value->getSize();
        }
        const auto cases = std::size_t{1} << bits;
        if (cases > case_limit)
            return {};
        std::set<std::uint64_t> found;
        for (std::size_t i = 0; i < cases; ++i) {
            unsigned shift{};
            for (const auto& [id, value] : free) {
                static_cast<void>(id);
                ctx.setConcreteVariableValue(
                    value, (i >> shift) & ((std::uint64_t{1} << value->getSize()) - 1));
                shift += value->getSize();
            }
            for (const auto id : derived) {
                const auto& input = inputs.at(id);
                const auto value =
                    read(regions, static_cast<std::uint64_t>(input.address->evaluate()), input.size);
                if (!value)
                    return {};
                ctx.setConcreteVariableValue(input.value, *value);
            }
            found.insert(static_cast<std::uint64_t>(goal->evaluate()));
            if (found.size() > 256)
                return {};
        }
        return branch_set{{found.begin(), found.end()}, cases};
    } catch (...) {
        return {};
    }
}
}
