#include "lifus/unlockall/mem/Triton.h"
#include <triton/context.hpp>
#include <triton/instruction.hpp>
#include <algorithm>
#include <string>
#include <cstring>

namespace uwu::sem {
namespace {
constexpr std::array<const char*, 16> gpr{"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9",
    "r10", "r11", "r12", "r13", "r14", "r15"};

bool same(const triton::ast::SharedAbstractNode& left, const triton::ast::SharedAbstractNode& right,
    unsigned& budget) {
    if (!budget || !left || !right)
        return false;
    --budget;
    if (left == right)
        return true;
    if (left->getType() != right->getType() || left->getBitvectorSize() != right->getBitvectorSize())
        return false;
    if (left->getType() == triton::ast::VARIABLE_NODE)
        return static_cast<triton::ast::VariableNode*>(left.get())->getSymbolicVariable()->getId() ==
            static_cast<triton::ast::VariableNode*>(right.get())->getSymbolicVariable()->getId();
    if (left->getType() == triton::ast::REFERENCE_NODE)
        return static_cast<triton::ast::ReferenceNode*>(left.get())->getSymbolicExpression()->getId() ==
            static_cast<triton::ast::ReferenceNode*>(right.get())->getSymbolicExpression()->getId();
    if (left->getType() == triton::ast::INTEGER_NODE)
        return static_cast<triton::ast::IntegerNode*>(left.get())->getInteger() ==
            static_cast<triton::ast::IntegerNode*>(right.get())->getInteger();
    const auto& a = left->getChildren();
    const auto& b = right->getChildren();
    if (a.empty() || a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (!same(a[i], b[i], budget))
            return false;
    return true;
}

bool is_zero(const triton::ast::SharedAbstractNode& node, unsigned& budget) {
    if (!budget || !node)
        return false;
    --budget;
    if (!node->isSymbolized())
        return node->evaluate() == 0;
    const auto& children = node->getChildren();
    switch (node->getType()) {
    case triton::ast::REFERENCE_NODE:
        return is_zero(
            static_cast<triton::ast::ReferenceNode*>(node.get())->getSymbolicExpression()->getAst(), budget);
    case triton::ast::BVSUB_NODE:
    case triton::ast::BVXOR_NODE:
        return children.size() == 2 && same(children[0], children[1], budget);
    case triton::ast::CONCAT_NODE:
        return !children.empty() && std::all_of(children.begin(), children.end(), [&](const auto& child) {
            return is_zero(child, budget);
        });
    case triton::ast::EXTRACT_NODE:
    case triton::ast::SX_NODE:
    case triton::ast::ZX_NODE:
        return !children.empty() && is_zero(children.back(), budget);
    default:
        return false;
    }
}
}

bool step(emulator::state& state, std::span<const std::byte> bytes, std::uint64_t address,
    std::span<const block> reads) noexcept {
    try {
        if (bytes.empty() || bytes.size() > 15 || reads.size() > 2)
            return false;
        triton::Context ctx(triton::arch::ARCH_X86_64);
        ctx.setMode(triton::modes::AST_OPTIMIZATIONS, true);
        for (unsigned i = 0; i < gpr.size(); ++i) {
            ctx.setConcreteRegisterValue(ctx.getRegister(gpr[i]), state.registers[i]);
            const auto value =
                triton::uint512(state.vectors[i][0]) | (triton::uint512(state.vectors[i][1]) << 64);
            ctx.setConcreteRegisterValue(ctx.getRegister("xmm" + std::to_string(i)), value);
        }
        ctx.setConcreteRegisterValue(ctx.getRegister("eflags"), state.flags);
        ctx.setConcreteRegisterValue(ctx.getRegister("gs"), state.gs);
        ctx.setConcreteRegisterValue(ctx.getRegister("rip"), address);
        for (const auto& block : reads) {
            if (block.bytes.empty() || block.bytes.size() > 16)
                return false;
            ctx.setConcreteMemoryAreaValue(block.address,
                reinterpret_cast<const triton::uint8*>(block.bytes.data()), block.bytes.size());
        }
        triton::arch::Instruction instruction;
        instruction.setAddress(address);
        instruction.setOpcode(bytes.data(), static_cast<triton::uint32>(bytes.size()));
        if (ctx.processing(instruction) != triton::arch::NO_FAULT || instruction.getSize() != bytes.size() ||
            !instruction.getStoreAccess().empty() ||
            ctx.getConcreteRegisterValue(ctx.getRegister("rip")) != address + bytes.size())
            return false;
        for (const auto& load : instruction.getLoadAccess()) {
            const auto at = load.first.getAddress();
            const auto size = load.first.getSize();
            if (std::none_of(reads.begin(), reads.end(), [&](const auto& block) {
                    return at >= block.address && at - block.address <= block.bytes.size() &&
                        size <= block.bytes.size() - (at - block.address);
                }))
                return false;
        }
        auto next = state;
        for (unsigned i = 0; i < gpr.size(); ++i) {
            next.registers[i] =
                static_cast<std::uint64_t>(ctx.getConcreteRegisterValue(ctx.getRegister(gpr[i])));
            const auto value = ctx.getConcreteRegisterValue(ctx.getRegister("xmm" + std::to_string(i)));
            next.vectors[i] = {static_cast<std::uint64_t>(value), static_cast<std::uint64_t>(value >> 64)};
        }
        next.flags = static_cast<std::uint64_t>(ctx.getConcreteRegisterValue(ctx.getRegister("eflags")));
        state = next;
        return true;
    } catch (...) {
        return false;
    }
}

std::optional<std::uint64_t> constant(const emulator::decoded& decoded, ZydisRegister output) noexcept {
    try {
        if (ZydisRegisterGetClass(output) != ZYDIS_REGCLASS_GPR64 || !decoded.instruction.length ||
            decoded.instruction.length > decoded.bytes.size() ||
            (decoded.instruction.cpu_flags && decoded.instruction.cpu_flags->tested))
            return {};
        ZydisDecoder decoder{};
        ZydisDecodedInstruction native{};
        std::array<ZydisDecodedOperand, ZYDIS_MAX_OPERAND_COUNT> operands{};
        ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(
                &decoder, decoded.bytes.data(), decoded.instruction.length, &native, operands.data())) ||
            std::memcmp(&native, &decoded.instruction, sizeof(native)) ||
            std::memcmp(operands.data(), decoded.operands.data(), sizeof(operands)))
            return {};
        for (unsigned i = 0; i < native.operand_count; ++i) {
            const auto& operand = operands[i];
            if (operand.type == ZYDIS_OPERAND_TYPE_MEMORY && operand.mem.type != ZYDIS_MEMOP_TYPE_AGEN)
                return {};
            if (operand.type == ZYDIS_OPERAND_TYPE_REGISTER &&
                (operand.actions & ZYDIS_OPERAND_ACTION_MASK_READ)) {
                const auto kind = ZydisRegisterGetClass(operand.reg.value);
                if (kind != ZYDIS_REGCLASS_GPR8 && kind != ZYDIS_REGCLASS_GPR16 &&
                    kind != ZYDIS_REGCLASS_GPR32 && kind != ZYDIS_REGCLASS_GPR64 &&
                    kind != ZYDIS_REGCLASS_XMM && kind != ZYDIS_REGCLASS_IP)
                    return {};
            }
        }
        triton::Context ctx(triton::arch::ARCH_X86_64);
        ctx.setMode(triton::modes::AST_OPTIMIZATIONS, true);
        for (const auto* name : gpr)
            ctx.symbolizeRegister(ctx.getRegister(name));
        for (unsigned i = 0; i < 16; ++i)
            ctx.symbolizeRegister(ctx.getRegister("xmm" + std::to_string(i)));
        ctx.setConcreteRegisterValue(ctx.getRegister("rip"), decoded.address);
        triton::arch::Instruction instruction;
        instruction.setAddress(decoded.address);
        instruction.setOpcode(decoded.bytes.data(), native.length);
        if (ctx.processing(instruction) != triton::arch::NO_FAULT || !instruction.getStoreAccess().empty() ||
            !instruction.getLoadAccess().empty())
            return {};
        const auto expression = ctx.getSymbolicRegister(ctx.getRegister(ZydisRegisterGetString(output)));
        if (!expression)
            return {};
        const auto ast = expression->getAst();
        if (!ast->isSymbolized())
            return static_cast<std::uint64_t>(ast->evaluate());
        unsigned budget = 512;
        return is_zero(ast, budget) ? std::optional<std::uint64_t>(0) : std::nullopt;
    } catch (...) {
        return {};
    }
}

bool zero(std::span<const std::byte> bytes, std::uint64_t address, unsigned xmm) noexcept {
    try {
        if (bytes.empty() || bytes.size() > 15 || xmm >= 16)
            return false;
        triton::Context ctx(triton::arch::ARCH_X86_64);
        ctx.setMode(triton::modes::AST_OPTIMIZATIONS, true);
        for (unsigned i = 0; i < 16; ++i)
            ctx.symbolizeRegister(ctx.getRegister("xmm" + std::to_string(i)));
        triton::arch::Instruction instruction;
        instruction.setAddress(address);
        instruction.setOpcode(bytes.data(), static_cast<triton::uint32>(bytes.size()));
        if (ctx.processing(instruction) != triton::arch::NO_FAULT || !instruction.getStoreAccess().empty() ||
            !instruction.getLoadAccess().empty())
            return false;
        const auto& expression = ctx.getSymbolicRegister(ctx.getRegister("xmm" + std::to_string(xmm)));
        if (!expression)
            return false;
        const auto ast = ctx.getAstContext()->extract(127, 0, expression->getAst());
        unsigned budget = 512;
        return is_zero(ast, budget);
    } catch (...) {
        return false;
    }
}
}
