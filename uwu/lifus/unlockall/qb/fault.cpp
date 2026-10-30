#include "lifus/unlockall/qb/Fault.h"
#include <triton/context.hpp>
#include <triton/instruction.hpp>
#include <set>
#include <string>

namespace uwu::sdk::locators {
namespace {
using ast_node = triton::ast::SharedAbstractNode;

ast_node dereference(ast_node value, unsigned& budget) {
    while (value && value->getType() == triton::ast::REFERENCE_NODE) {
        if (!budget)
            return {};
        --budget;
        value = static_cast<triton::ast::ReferenceNode*>(value.get())->getSymbolicExpression()->getAst();
    }
    return value;
}

bool identical(ast_node a, ast_node b, unsigned& budget) {
    if (!budget || !(a = dereference(a, budget)) || !(b = dereference(b, budget)))
        return false;
    --budget;
    if (a == b)
        return true;
    if (a->getType() != b->getType() || a->getBitvectorSize() != b->getBitvectorSize())
        return false;
    if (a->getType() == triton::ast::VARIABLE_NODE)
        return static_cast<triton::ast::VariableNode*>(a.get())->getSymbolicVariable()->getId() ==
            static_cast<triton::ast::VariableNode*>(b.get())->getSymbolicVariable()->getId();
    if (a->getType() == triton::ast::INTEGER_NODE)
        return triton::ast::getInteger<triton::uint512>(a) == triton::ast::getInteger<triton::uint512>(b);
    const auto& x = a->getChildren();
    const auto& y = b->getChildren();
    if (x.empty() || x.size() != y.size())
        return false;
    for (std::size_t i = 0; i < x.size(); ++i)
        if (!identical(x[i], y[i], budget))
            return false;
    return true;
}

std::optional<std::uint64_t> constant(ast_node value, unsigned& budget) {
    if (!budget || !(value = dereference(value, budget)))
        return {};
    --budget;
    const auto width = value->getBitvectorSize();
    if (!width || width > 64)
        return {};
    const auto mask = width == 64 ? ~0ull : (1ull << width) - 1;
    if (!value->isSymbolized())
        return static_cast<std::uint64_t>(value->evaluate()) & mask;
    const auto& c = value->getChildren();
    const auto type = value->getType();
    if ((type == triton::ast::BVXOR_NODE || type == triton::ast::BVSUB_NODE) && c.size() == 2 &&
        identical(c[0], c[1], budget))
        return 0;
    if (type == triton::ast::EXTRACT_NODE && c.size() == 3) {
        const auto low = triton::ast::getInteger<unsigned>(c[1]);
        const auto source = constant(c[2], budget);
        if (source && low < 64)
            return (*source >> low) & mask;
        return {};
    }
    if ((type == triton::ast::ZX_NODE || type == triton::ast::SX_NODE) && c.size() == 2) {
        const auto source = constant(c[1], budget);
        if (!source)
            return {};
        auto result = *source;
        const auto old_width = c[1]->getBitvectorSize();
        if (type == triton::ast::SX_NODE && old_width < 64 && (result & (1ull << (old_width - 1))))
            result |= (~0ull) << old_width;
        return result & mask;
    }
    if (type == triton::ast::CONCAT_NODE) {
        std::uint64_t result{};
        unsigned offset{};
        for (auto it = c.rbegin(); it != c.rend(); ++it) {
            const auto source = constant(*it, budget);
            if (!source || offset >= 64)
                return {};
            result |= *source << offset;
            offset += (*it)->getBitvectorSize();
        }
        return result & mask;
    }
    if (c.size() == 2 &&
        (type == triton::ast::BVADD_NODE || type == triton::ast::BVSUB_NODE ||
            type == triton::ast::BVXOR_NODE || type == triton::ast::BVAND_NODE ||
            type == triton::ast::BVOR_NODE)) {
        const auto a = constant(c[0], budget), b = constant(c[1], budget);
        if (type == triton::ast::BVAND_NODE && ((a && !*a) || (b && !*b)))
            return 0;
        if (!a || !b)
            return {};
        if (type == triton::ast::BVADD_NODE)
            return (*a + *b) & mask;
        if (type == triton::ast::BVSUB_NODE)
            return (*a - *b) & mask;
        if (type == triton::ast::BVXOR_NODE)
            return (*a ^ *b) & mask;
        if (type == triton::ast::BVAND_NODE)
            return (*a & *b) & mask;
        return (*a | *b) & mask;
    }
    return {};
}

bool fixed_address(const triton::arch::MemoryAccess& access) {
    unsigned budget = 4096;
    const auto value = constant(access.getLeaAst(), budget);
    return value && *value == access.getAddress();
}

bool bound(const uwu::emulator::decoded& instruction) {
    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    ZydisDecodedInstruction d{};
    std::array<ZydisDecodedOperand, ZYDIS_MAX_OPERAND_COUNT> operands{};
    if (!instruction.instruction.length || instruction.instruction.length > 15 ||
        !ZYAN_SUCCESS(ZydisDecoderDecodeFull(
            &decoder, instruction.bytes.data(), instruction.instruction.length, &d, operands.data())) ||
        d.length != instruction.instruction.length || d.mnemonic != instruction.instruction.mnemonic ||
        d.operand_count != instruction.instruction.operand_count ||
        d.meta.category != instruction.instruction.meta.category)
        return false;
    for (unsigned i = 0; i < d.operand_count; ++i) {
        const auto& a = operands[i];
        const auto& b = instruction.operands[i];
        if (a.type != b.type || a.size != b.size || a.actions != b.actions)
            return false;
        if (a.type == ZYDIS_OPERAND_TYPE_REGISTER && a.reg.value != b.reg.value)
            return false;
        if (a.type == ZYDIS_OPERAND_TYPE_IMMEDIATE &&
            (a.imm.value.u != b.imm.value.u || a.imm.is_relative != b.imm.is_relative))
            return false;
        if (a.type == ZYDIS_OPERAND_TYPE_MEMORY &&
            (a.mem.base != b.mem.base || a.mem.index != b.mem.index || a.mem.segment != b.mem.segment ||
                a.mem.disp.value != b.mem.disp.value || a.mem.scale != b.mem.scale))
            return false;
    }
    return true;
}
}

fault_analysis normal_execution_faults(
    const uwu::emulator::program& body, std::uint64_t user_min, std::uint64_t user_end) {
    fault_analysis result;
    if (body.instructions.empty() || body.instructions.size() > 65536 || user_min >= user_end)
        return result;
    for (const auto& instruction : body.instructions)
        if (!bound(instruction))
            return result;
    result.opcode_binding = true;
    std::set<std::uint64_t> starts{body.instructions.front().address};
    for (const auto& d : body.instructions) {
        const auto category = d.instruction.meta.category;
        if (category == ZYDIS_CATEGORY_CALL || category == ZYDIS_CATEGORY_RET ||
            category == ZYDIS_CATEGORY_COND_BR || category == ZYDIS_CATEGORY_UNCOND_BR)
            starts.insert(d.address + d.instruction.length);
        if ((category == ZYDIS_CATEGORY_COND_BR || category == ZYDIS_CATEGORY_UNCOND_BR) &&
            d.operands[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE && d.operands[0].imm.is_relative)
            starts.insert(d.address + d.instruction.length + d.operands[0].imm.value.s);
    }
    for (std::size_t first = 0; first < body.instructions.size();) {
        std::size_t end = first + 1;
        while (end < body.instructions.size() && !starts.contains(body.instructions[end].address))
            ++end;
        ++result.blocks;
        if (end - first > 512) {
            ++result.unsupported;
            first = end;
            continue;
        }
        try {
            triton::Context ctx(triton::arch::ARCH_X86_64);
            ctx.setMode(triton::modes::AST_OPTIMIZATIONS, true);
            for (const auto name : {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10",
                     "r11", "r12", "r13", "r14", "r15"})
                ctx.symbolizeRegister(ctx.getRegister(name), name);
            for (const auto flag : {"cf", "pf", "af", "zf", "sf", "tf", "if", "df", "of"})
                ctx.symbolizeRegister(ctx.getRegister(flag), flag);
            for (unsigned i = 0; i < 16; ++i)
                ctx.symbolizeRegister(ctx.getRegister("xmm" + std::to_string(i)), "xmm_" + std::to_string(i));
            std::set<std::uint64_t> defined;
            std::vector<std::uint64_t> prefix;
            for (std::size_t i = first; i < end; ++i) {
                const auto& d = body.instructions[i];
                const auto category = d.instruction.meta.category;
                if (category == ZYDIS_CATEGORY_CALL || category == ZYDIS_CATEGORY_RET ||
                    category == ZYDIS_CATEGORY_COND_BR || category == ZYDIS_CATEGORY_UNCOND_BR)
                    break;
                bool segmented{};
                for (unsigned k = 0; k < d.instruction.operand_count; ++k)
                    segmented |= d.operands[k].type == ZYDIS_OPERAND_TYPE_MEMORY &&
                        (d.operands[k].mem.segment == ZYDIS_REGISTER_FS ||
                            d.operands[k].mem.segment == ZYDIS_REGISTER_GS);
                if (segmented) {
                    ++result.unknown_memory;
                    break;
                }
                bool unknown_address{};
                if (d.instruction.mnemonic != ZYDIS_MNEMONIC_LEA)
                    for (unsigned k = 0; k < d.instruction.operand_count; ++k) {
                        const auto& op = d.operands[k];
                        if (op.type != ZYDIS_OPERAND_TYPE_MEMORY ||
                            !(op.actions & ZYDIS_OPERAND_ACTION_MASK_READ))
                            continue;
                        std::uint64_t address = static_cast<std::uint64_t>(op.mem.disp.value);
                        if (op.mem.base == ZYDIS_REGISTER_RIP)
                            address += d.address + d.instruction.length;
                        else if (op.mem.base != ZYDIS_REGISTER_NONE) {
                            unsigned budget = 4096;
                            const auto value = constant(
                                ctx.getRegisterAst(ctx.getRegister(ZydisRegisterGetString(op.mem.base))),
                                budget);
                            if (!value) {
                                unknown_address = true;
                                break;
                            }
                            address += *value;
                        }
                        if (op.mem.index != ZYDIS_REGISTER_NONE) {
                            unsigned budget = 4096;
                            const auto value = constant(
                                ctx.getRegisterAst(ctx.getRegister(ZydisRegisterGetString(op.mem.index))),
                                budget);
                            if (!value) {
                                unknown_address = true;
                                break;
                            }
                            address += *value * op.mem.scale;
                        }
                        if (d.instruction.address_width == 32)
                            address &= 0xffffffffu;
                        if (!op.size || op.size > 512 || op.size % 8) {
                            unknown_address = true;
                            break;
                        }
                        for (unsigned byte = 0; byte < static_cast<unsigned>(op.size / 8); ++byte) {
                            const auto at = address + byte;
                            if (defined.insert(at).second) {
                                ctx.setConcreteMemoryValue(at, 0, false);
                                ctx.symbolizeMemory(
                                    triton::arch::MemoryAccess(at, 1), "unknown_byte_" + std::to_string(at));
                            }
                        }
                    }
                if (unknown_address) {
                    ++result.unknown_memory;
                    break;
                }
                triton::arch::Instruction instruction;
                instruction.setAddress(d.address);
                instruction.setOpcode(d.bytes.data(), d.instruction.length);
                if (ctx.processing(instruction) != triton::arch::NO_FAULT) {
                    ++result.unsupported;
                    break;
                }
                bool unknown{};
                for (const auto& access : instruction.getLoadAccess()) {
                    unknown |= !fixed_address(access.first);
                    for (unsigned byte = 0; byte < access.first.getSize(); ++byte)
                        unknown |= !defined.contains(access.first.getAddress() + byte);
                }
                for (const auto& access : instruction.getStoreAccess())
                    unknown |= !fixed_address(access.first);
                if (unknown) {
                    ++result.unknown_memory;
                    break;
                }
                prefix.push_back(d.address);
                bool mandatory_store{};
                for (unsigned k = 0; k < d.instruction.operand_count; ++k)
                    mandatory_store |= d.operands[k].type == ZYDIS_OPERAND_TYPE_MEMORY &&
                        (d.operands[k].actions & ZYDIS_OPERAND_ACTION_WRITE) &&
                        !(d.operands[k].actions & ZYDIS_OPERAND_ACTION_CONDWRITE);
                bool fault{};
                if (mandatory_store)
                    for (const auto& access : instruction.getStoreAccess()) {
                        const auto at = access.first.getAddress();
                        const auto size = access.first.getSize();
                        if (at < user_min || at >= user_end || size > user_end - at) {
                            result.faults.push_back(
                                {body.instructions[first].address, d.address, at, size, prefix});
                            fault = true;
                        }
                    }
                if (fault)
                    break;
                for (const auto& access : instruction.getStoreAccess())
                    for (unsigned j = 0; j < access.first.getSize(); ++j)
                        defined.insert(access.first.getAddress() + j);
            }
        } catch (const std::exception&) {
            ++result.unsupported;
        }
        first = end;
    }
    return result;
}
}
