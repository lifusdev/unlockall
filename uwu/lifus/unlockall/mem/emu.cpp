#include "lifus/unlockall/mem/Emu.h"
#include "lifus/unlockall/mem/Shadow.h"
#include "lifus/unlockall/mem/Triton.h"
#include <algorithm>
#include <bit>
#include <cstring>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <intrin.h>

namespace uwu {
namespace {
struct fault {
    emulation_status status;
};

constexpr std::uint64_t cf = 1, pf = 4, af = 16, zf = 64, sf = 128, of = 2048;

std::uint64_t mask(unsigned bits_value) {
    if (!bits_value || bits_value > 64)
        throw fault{emulation_status::invalid_operand};
    return bits_value == 64 ? ~0ull : (1ull << bits_value) - 1;
}

struct register_part {
    unsigned index, bits, shift;
};

register_part part(ZydisRegister reg) {
    const auto enclosing = ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, reg);
    if (ZydisRegisterGetClass(enclosing) != ZYDIS_REGCLASS_GPR64)
        throw fault{emulation_status::invalid_operand};
    const auto id = ZydisRegisterGetId(enclosing);
    const unsigned width = ZydisRegisterGetWidth(ZYDIS_MACHINE_MODE_LONG_64, reg);
    if (id < 0 || id >= 16 || !width || width > 64)
        throw fault{emulation_status::invalid_operand};
    const unsigned shift = (reg == ZYDIS_REGISTER_AH || reg == ZYDIS_REGISTER_BH ||
                               reg == ZYDIS_REGISTER_CH || reg == ZYDIS_REGISTER_DH)
        ? 8
        : 0;
    return {static_cast<unsigned>(id), width, shift};
}

bool contains(std::uint64_t base, std::size_t size_value, std::uint64_t at, std::size_t bytes_value) {
    return at >= base && at - base <= size_value &&
        bytes_value <= size_value - static_cast<std::size_t>(at - base);
}
}

std::optional<emulator::program> emulator::decode(
    std::span<const std::byte> bytes_value, std::uint64_t address_value, bool prefix) {
    if (bytes_value.empty() || bytes_value.size() > 0x10000 || address_value > ~0ull - bytes_value.size())
        return std::nullopt;
    ZydisDecoder decoder{};
    if (!ZYAN_SUCCESS(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64)))
        return std::nullopt;
    program result_value{address_value, address_value + bytes_value.size(), {}};
    for (std::size_t offset = 0; offset < bytes_value.size();) {
        decoded instruction_value{};
        instruction_value.address = address_value + offset;
        if (!ZYAN_SUCCESS(
                ZydisDecoderDecodeFull(&decoder, bytes_value.data() + offset, bytes_value.size() - offset,
                    &instruction_value.instruction, instruction_value.operands.data())) ||
            !instruction_value.instruction.length) {
            if (!prefix || result_value.instructions.empty())
                return std::nullopt;
            result_value.end = address_value + offset;
            break;
        }
        std::memcpy(instruction_value.bytes.data(), bytes_value.data() + offset,
            instruction_value.instruction.length);
        offset += instruction_value.instruction.length;
        result_value.instructions.push_back(instruction_value);
    }
    return result_value;
}

void emulator::reset(std::uint64_t teb) noexcept {
    registers_.fill(0);
    vectors_.fill({});
    stack_.fill(std::byte{});
    initialized_.reset();
    shadow_ = nullptr;
    inputs_.clear();
    ip_ = next_ = current_ = 0;
    gs_ = teb;
    flags_ = 2;
    address_width_ = 64;
    registers_[4] = stack_base + 0x8000;
}

void emulator::set(ZydisRegister reg, std::uint64_t value_data) {
    if (ZydisRegisterGetClass(reg) == ZYDIS_REGCLASS_XMM) {
        vectors_.at(ZydisRegisterGetId(reg))[0] = value_data;
        return;
    }
    const auto r = part(reg);
    value_data &= mask(r.bits);
    auto& destination = registers_[r.index];
    if (r.bits >= 32)
        destination = value_data;
    else
        destination = (destination & ~(mask(r.bits) << r.shift)) | (value_data << r.shift);
}

std::uint64_t emulator::get(ZydisRegister reg) const {
    if (ZydisRegisterGetClass(reg) == ZYDIS_REGCLASS_XMM)
        return vectors_.at(ZydisRegisterGetId(reg))[0];
    if (reg == ZYDIS_REGISTER_RIP)
        return next_;
    if (reg == ZYDIS_REGISTER_EIP)
        return static_cast<std::uint32_t>(next_);
    if (reg == ZYDIS_REGISTER_NONE)
        return 0;
    const auto r = part(reg);
    return (registers_[r.index] >> r.shift) & mask(r.bits);
}

void emulator::stack(std::uint32_t offset, std::uint64_t value_data) {
    store(get(ZYDIS_REGISTER_RSP) + offset, 64, value_data);
}

std::uint64_t emulator::stack(std::uint32_t offset) const {
    return load(get(ZYDIS_REGISTER_RSP) + offset, 64);
}

void emulator::input(std::uint64_t at, std::span<const std::byte> bytes_value) {
    if (shadow_ || bytes_value.empty() || at > ~0ull - bytes_value.size() ||
        (at < stack_base + stack_.size() && at + bytes_value.size() > stack_base))
        throw std::invalid_argument("invalid emulator input");
    for (const auto& r : inputs_)
        if (at < r.address + r.bytes.size() && r.address < at + bytes_value.size())
            throw std::invalid_argument("overlapping emulator input");
    inputs_.push_back({at, bytes_value});
}

void emulator::shadow(shadow_memory& memory) {
    if (!inputs_.empty() || shadow_ || current_ || ip_ || next_)
        throw std::invalid_argument("emulator memory already supplied");
    shadow_ = &memory;
}

std::uint64_t emulator::address(const ZydisDecodedOperand& operand, bool segment) const {
    if (operand.type != ZYDIS_OPERAND_TYPE_MEMORY ||
        (operand.mem.type != ZYDIS_MEMOP_TYPE_MEM && (segment || operand.mem.type != ZYDIS_MEMOP_TYPE_AGEN)))
        throw fault{emulation_status::invalid_operand};
    auto at = get(operand.mem.base) + get(operand.mem.index) * operand.mem.scale;
    if (operand.mem.disp.has_displacement)
        at += static_cast<std::uint64_t>(operand.mem.disp.value);
    at &= mask(address_width_);
    if (segment) {
        if (operand.mem.segment == ZYDIS_REGISTER_GS)
            at += gs_;
        else if (operand.mem.segment == ZYDIS_REGISTER_FS)
            throw fault{emulation_status::unsupported};
    }
    return at;
}

std::uint64_t emulator::load(std::uint64_t at, unsigned bits_value) const {
    if (bits_value % 8)
        throw fault{emulation_status::invalid_operand};
    const auto bytes_value = bits_value / 8;
    mask(bits_value);
    std::uint64_t result_value{};
    const auto report_value = [&](DWORD error_value, bool supplied = false, bool stack_value = false) {
        if (observer_.read)
            observer_.read(observer_.context,
                {current_, at, result_value, bits_value, error_value, supplied, stack_value});
    };
    if (contains(stack_base, stack_.size(), at, bytes_value)) {
        if (shadow_)
            for (std::size_t i = 0; i < bytes_value; ++i)
                if (!initialized_.test(at - stack_base + i)) {
                    report_value(ERROR_PARTIAL_COPY, false, true);
                    throw fault{emulation_status::read_failed};
                }
        std::memcpy(&result_value, stack_.data() + at - stack_base, bytes_value);
        report_value(ERROR_SUCCESS, false, true);
    } else if (shadow_) {
        if (at < stack_base + stack_.size() && at + bytes_value > stack_base) {
            report_value(ERROR_INVALID_ADDRESS);
            throw fault{emulation_status::read_failed};
        }
        const auto read =
            shadow_->read_bytes(at, std::as_writable_bytes(std::span(&result_value, 1)).first(bytes_value));
        report_value(read.error, true);
        if (!read.complete() || read.transferred != bytes_value)
            throw fault{read.error == ERROR_NOT_ENOUGH_QUOTA ? emulation_status::budget_exhausted
                                                             : emulation_status::read_failed};
    } else {
        const auto found = std::find_if(inputs_.begin(), inputs_.end(),
            [&](const region& r) { return contains(r.address, r.bytes.size(), at, bytes_value); });
        if (found != inputs_.end()) {
            std::memcpy(&result_value, found->bytes.data() + at - found->address, bytes_value);
            report_value(ERROR_SUCCESS, true);
        } else {
            if (at <= 0x1000 || at >= 0x800000000000ull || bytes_value > 0x800000000000ull - at ||
                (at < stack_base + stack_.size() && at + bytes_value > stack_base)) {
                report_value(ERROR_INVALID_ADDRESS);
                throw fault{emulation_status::read_failed};
            }
            const auto got = process_.read_bytes(static_cast<std::uintptr_t>(at),
                std::as_writable_bytes(std::span(&result_value, 1)).first(bytes_value));
            report_value(got.error);
            if (!got.complete())
                throw fault{emulation_status::read_failed};
        }
    }
    return result_value;
}

void emulator::store(std::uint64_t at, unsigned bits_value, std::uint64_t value_data) {
    if (bits_value % 8)
        throw fault{emulation_status::invalid_operand};
    const auto bytes_value = bits_value / 8;
    mask(bits_value);
    if (!contains(stack_base, stack_.size(), at, bytes_value)) {
        if (!shadow_ || (at < stack_base + stack_.size() && at + bytes_value > stack_base))
            throw fault{emulation_status::write_blocked};
        const auto error =
            shadow_->write_bytes(current_, at, std::as_bytes(std::span(&value_data, 1)).first(bytes_value));
        if (error)
            throw fault{error == ERROR_NOT_ENOUGH_QUOTA ? emulation_status::budget_exhausted
                    : error == ERROR_ACCESS_DENIED      ? emulation_status::write_blocked
                                                        : emulation_status::read_failed};
        return;
    }
    std::memcpy(stack_.data() + at - stack_base, &value_data, bytes_value);
    for (std::size_t i = 0; i < bytes_value; ++i)
        initialized_.set(at - stack_base + i);
}

std::uint64_t emulator::value(const ZydisDecodedOperand& operand) const {
    switch (operand.type) {
    case ZYDIS_OPERAND_TYPE_REGISTER:
        return get(operand.reg.value);
    case ZYDIS_OPERAND_TYPE_MEMORY:
        return load(address(operand), operand.size);
    case ZYDIS_OPERAND_TYPE_IMMEDIATE:
        if (operand.imm.is_relative)
            return next_ + static_cast<std::uint64_t>(operand.imm.value.s);
        return operand.imm.value.u;
    default:
        throw fault{emulation_status::invalid_operand};
    }
}

void emulator::write(const ZydisDecodedOperand& operand, std::uint64_t value_data) {
    if (operand.type == ZYDIS_OPERAND_TYPE_REGISTER)
        set(operand.reg.value, value_data);
    else if (operand.type == ZYDIS_OPERAND_TYPE_MEMORY)
        store(address(operand), operand.size, value_data);
    else
        throw fault{emulation_status::invalid_operand};
}

void emulator::logical(std::uint64_t result_value, unsigned bits_value) {
    result_value &= mask(bits_value);
    flags_ &= ~(cf | of | sf | zf | pf | af);
    if (!result_value)
        flags_ |= zf;
    if (result_value & (1ull << (bits_value - 1)))
        flags_ |= sf;
    if ((std::popcount(static_cast<unsigned>(result_value & 0xff)) & 1) == 0)
        flags_ |= pf;
}

void emulator::arithmetic(
    std::uint64_t a, std::uint64_t b, std::uint64_t result_value, unsigned bits_value, bool subtract) {
    const auto limit = mask(bits_value), sign = 1ull << (bits_value - 1);
    a &= limit;
    b &= limit;
    result_value &= limit;
    logical(result_value, bits_value);
    if (subtract ? a < b : result_value < a)
        flags_ |= cf;
    if (((subtract ? (a ^ b) : ~(a ^ b)) & (a ^ result_value) & sign) != 0)
        flags_ |= of;
    if ((a ^ b ^ result_value) & 0x10)
        flags_ |= af;
}

std::optional<bool> emulator::condition(ZydisMnemonic m) const {
    const bool c = (flags_ & cf) != 0, z = (flags_ & zf) != 0, s = (flags_ & sf) != 0, o = (flags_ & of) != 0,
               p = (flags_ & pf) != 0;
    switch (m) {
    case ZYDIS_MNEMONIC_JB:
    case ZYDIS_MNEMONIC_CMOVB:
    case ZYDIS_MNEMONIC_SETB:
        return c;
    case ZYDIS_MNEMONIC_JBE:
    case ZYDIS_MNEMONIC_CMOVBE:
    case ZYDIS_MNEMONIC_SETBE:
        return c || z;
    case ZYDIS_MNEMONIC_JNB:
    case ZYDIS_MNEMONIC_CMOVNB:
    case ZYDIS_MNEMONIC_SETNB:
        return !c;
    case ZYDIS_MNEMONIC_JNBE:
    case ZYDIS_MNEMONIC_CMOVNBE:
    case ZYDIS_MNEMONIC_SETNBE:
        return !c && !z;
    case ZYDIS_MNEMONIC_JZ:
    case ZYDIS_MNEMONIC_CMOVZ:
    case ZYDIS_MNEMONIC_SETZ:
        return z;
    case ZYDIS_MNEMONIC_JNZ:
    case ZYDIS_MNEMONIC_CMOVNZ:
    case ZYDIS_MNEMONIC_SETNZ:
        return !z;
    case ZYDIS_MNEMONIC_JS:
    case ZYDIS_MNEMONIC_CMOVS:
    case ZYDIS_MNEMONIC_SETS:
        return s;
    case ZYDIS_MNEMONIC_JNS:
    case ZYDIS_MNEMONIC_CMOVNS:
    case ZYDIS_MNEMONIC_SETNS:
        return !s;
    case ZYDIS_MNEMONIC_JO:
    case ZYDIS_MNEMONIC_CMOVO:
    case ZYDIS_MNEMONIC_SETO:
        return o;
    case ZYDIS_MNEMONIC_JNO:
    case ZYDIS_MNEMONIC_CMOVNO:
    case ZYDIS_MNEMONIC_SETNO:
        return !o;
    case ZYDIS_MNEMONIC_JP:
    case ZYDIS_MNEMONIC_CMOVP:
    case ZYDIS_MNEMONIC_SETP:
        return p;
    case ZYDIS_MNEMONIC_JNP:
    case ZYDIS_MNEMONIC_CMOVNP:
    case ZYDIS_MNEMONIC_SETNP:
        return !p;
    case ZYDIS_MNEMONIC_JL:
    case ZYDIS_MNEMONIC_CMOVL:
    case ZYDIS_MNEMONIC_SETL:
        return s != o;
    case ZYDIS_MNEMONIC_JNL:
    case ZYDIS_MNEMONIC_CMOVNL:
    case ZYDIS_MNEMONIC_SETNL:
        return s == o;
    case ZYDIS_MNEMONIC_JLE:
    case ZYDIS_MNEMONIC_CMOVLE:
    case ZYDIS_MNEMONIC_SETLE:
        return z || s != o;
    case ZYDIS_MNEMONIC_JNLE:
    case ZYDIS_MNEMONIC_CMOVNLE:
    case ZYDIS_MNEMONIC_SETNLE:
        return !z && s == o;
    default:
        return std::nullopt;
    }
}

void emulator::step(const decoded& d) {
    const auto& i = d.instruction;
    const auto& x = d.operands[0];
    const auto& y = d.operands[1];
    const auto m = i.mnemonic;
    if (i.attributes &
        (ZYDIS_ATTRIB_HAS_LOCK | ZYDIS_ATTRIB_HAS_REP | ZYDIS_ATTRIB_HAS_REPE | ZYDIS_ATTRIB_HAS_REPNE))
        throw fault{emulation_status::unsupported};
    if (const auto take = condition(m)) {
        if (i.meta.category == ZYDIS_CATEGORY_COND_BR) {
            if (*take)
                ip_ = value(x);
        } else if (i.meta.category == ZYDIS_CATEGORY_CMOV) {
            const auto source = value(y);
            if (*take)
                write(x, source);
            else if (x.type == ZYDIS_OPERAND_TYPE_REGISTER && x.size == 32)
                write(x, value(x));
        } else if (i.meta.category == ZYDIS_CATEGORY_SETCC)
            write(x, *take ? 1 : 0);
        else
            throw fault{emulation_status::unsupported};
        return;
    }
    switch (m) {
    case ZYDIS_MNEMONIC_PSHUFD:
    case ZYDIS_MNEMONIC_PUNPCKLQDQ:
    case ZYDIS_MNEMONIC_PUNPCKHQDQ: {
        ZydisDecoder decoder{};
        ZydisDecodedInstruction native{};
        std::array<ZydisDecodedOperand, ZYDIS_MAX_OPERAND_COUNT> operands{};
        ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
        if (!ZYAN_SUCCESS(
                ZydisDecoderDecodeFull(&decoder, d.bytes.data(), i.length, &native, operands.data())) ||
            std::memcmp(&native, &i, sizeof(native)) != 0 ||
            std::memcmp(operands.data(), d.operands.data(), sizeof(operands)) != 0)
            throw fault{emulation_status::unsupported};
        if (y.type == ZYDIS_OPERAND_TYPE_MEMORY && y.mem.segment == ZYDIS_REGISTER_FS)
            throw fault{emulation_status::unsupported};
        const unsigned source_bits =
            m != ZYDIS_MNEMONIC_PSHUFD && y.type == ZYDIS_OPERAND_TYPE_REGISTER ? 64 : 128;
        if (x.type != ZYDIS_OPERAND_TYPE_REGISTER || x.size != 128 ||
            ZydisRegisterGetClass(x.reg.value) != ZYDIS_REGCLASS_XMM || y.size != source_bits ||
            (y.type != ZYDIS_OPERAND_TYPE_MEMORY &&
                (y.type != ZYDIS_OPERAND_TYPE_REGISTER ||
                    ZydisRegisterGetClass(y.reg.value) != ZYDIS_REGCLASS_XMM)))
            throw fault{emulation_status::invalid_operand};
        const auto source = y.type == ZYDIS_OPERAND_TYPE_REGISTER
            ? vectors_.at(ZydisRegisterGetId(y.reg.value))
            : std::array<std::uint64_t, 2>{
                  load(address(y), 64), source_bits == 128 ? load(address(y) + 8, 64) : 0};
        auto& destination = vectors_.at(ZydisRegisterGetId(x.reg.value));
        if (m == ZYDIS_MNEMONIC_PSHUFD) {
            const auto lanes = std::bit_cast<std::array<std::uint32_t, 4>>(source);
            const auto control = d.operands[2].imm.value.u;
            destination =
                std::bit_cast<std::array<std::uint64_t, 2>>(std::array<std::uint32_t, 4>{lanes[control & 3],
                    lanes[(control >> 2) & 3], lanes[(control >> 4) & 3], lanes[(control >> 6) & 3]});
        } else {
            const unsigned lane = m == ZYDIS_MNEMONIC_PUNPCKLQDQ ? 0 : 1;
            destination = {destination[lane], source[lane]};
        }
        return;
    }
    case ZYDIS_MNEMONIC_SHUFPS:
    case ZYDIS_MNEMONIC_ADDPS:
    case ZYDIS_MNEMONIC_SUBPS:
    case ZYDIS_MNEMONIC_MULPS:
    case ZYDIS_MNEMONIC_UNPCKLPS:
    case ZYDIS_MNEMONIC_UNPCKHPS:
    case ZYDIS_MNEMONIC_UNPCKHPD:
    case ZYDIS_MNEMONIC_MOVLHPS: {
        const auto vector_value = [](const ZydisDecodedOperand& operand) {
            return operand.type == ZYDIS_OPERAND_TYPE_REGISTER &&
                ZydisRegisterGetClass(operand.reg.value) == ZYDIS_REGCLASS_XMM;
        };
        if (!vector_value(x) ||
            (!vector_value(y) && (y.type != ZYDIS_OPERAND_TYPE_MEMORY || y.size != 128)) ||
            (m == ZYDIS_MNEMONIC_MOVLHPS && !vector_value(y)))
            throw fault{emulation_status::invalid_operand};
        auto& target_value = vectors_.at(ZydisRegisterGetId(x.reg.value));
        const auto source = vector_value(y)
            ? vectors_.at(ZydisRegisterGetId(y.reg.value))
            : std::array<std::uint64_t, 2>{load(address(y), 64), load(address(y) + 8, 64)};
        if (m == ZYDIS_MNEMONIC_MOVLHPS) {
            target_value[1] = source[0];
        } else if (m == ZYDIS_MNEMONIC_UNPCKHPD) {
            target_value = {target_value[1], source[1]};
        } else if (m == ZYDIS_MNEMONIC_UNPCKLPS || m == ZYDIS_MNEMONIC_UNPCKHPS) {
            const auto a = std::bit_cast<std::array<std::uint32_t, 4>>(target_value);
            const auto b = std::bit_cast<std::array<std::uint32_t, 4>>(source);
            const unsigned lane = m == ZYDIS_MNEMONIC_UNPCKLPS ? 0 : 2;
            target_value = std::bit_cast<std::array<std::uint64_t, 2>>(
                std::array<std::uint32_t, 4>{a[lane], b[lane], a[lane + 1], b[lane + 1]});
        } else if (m == ZYDIS_MNEMONIC_SHUFPS) {
            if (d.operands[2].type != ZYDIS_OPERAND_TYPE_IMMEDIATE)
                throw fault{emulation_status::invalid_operand};
            const auto immediate = d.operands[2].imm.value.u;
            const auto a = std::bit_cast<std::array<std::uint32_t, 4>>(target_value);
            const auto b = std::bit_cast<std::array<std::uint32_t, 4>>(source);
            target_value = std::bit_cast<std::array<std::uint64_t, 2>>(std::array<std::uint32_t, 4>{
                a[immediate & 3], a[(immediate >> 2) & 3], b[(immediate >> 4) & 3], b[(immediate >> 6) & 3]});
        } else {
            auto a = std::bit_cast<std::array<float, 4>>(target_value);
            const auto b = std::bit_cast<std::array<float, 4>>(source);
            for (unsigned lane = 0; lane < a.size(); ++lane)
                a[lane] = m == ZYDIS_MNEMONIC_ADDPS ? a[lane] + b[lane]
                    : m == ZYDIS_MNEMONIC_SUBPS     ? a[lane] - b[lane]
                                                    : a[lane] * b[lane];
            target_value = std::bit_cast<std::array<std::uint64_t, 2>>(a);
        }
        return;
    }
    case ZYDIS_MNEMONIC_PXOR:
    case ZYDIS_MNEMONIC_XORPS: {
        if (x.type != ZYDIS_OPERAND_TYPE_REGISTER || x.size != 128 ||
            ZydisRegisterGetClass(x.reg.value) != ZYDIS_REGCLASS_XMM || y.size != 128 ||
            (y.type != ZYDIS_OPERAND_TYPE_MEMORY &&
                (y.type != ZYDIS_OPERAND_TYPE_REGISTER ||
                    ZydisRegisterGetClass(y.reg.value) != ZYDIS_REGCLASS_XMM)))
            throw fault{emulation_status::invalid_operand};
        auto& target_value = vectors_.at(ZydisRegisterGetId(x.reg.value));
        const auto source = y.type == ZYDIS_OPERAND_TYPE_REGISTER
            ? vectors_.at(ZydisRegisterGetId(y.reg.value))
            : std::array<std::uint64_t, 2>{load(address(y), 64), load(address(y) + 8, 64)};
        target_value[0] ^= source[0];
        target_value[1] ^= source[1];
        return;
    }
    case ZYDIS_MNEMONIC_MOVAPS:
    case ZYDIS_MNEMONIC_MOVUPS:
    case ZYDIS_MNEMONIC_MOVDQA:
    case ZYDIS_MNEMONIC_MOVDQU: {
        std::array<std::uint64_t, 2> data{};
        if (y.type == ZYDIS_OPERAND_TYPE_REGISTER && ZydisRegisterGetClass(y.reg.value) == ZYDIS_REGCLASS_XMM)
            data = vectors_.at(ZydisRegisterGetId(y.reg.value));
        else if (y.type == ZYDIS_OPERAND_TYPE_MEMORY) {
            const auto at = address(y);
            data = {load(at, 64), load(at + 8, 64)};
        } else
            throw fault{emulation_status::invalid_operand};
        if (x.type == ZYDIS_OPERAND_TYPE_REGISTER && ZydisRegisterGetClass(x.reg.value) == ZYDIS_REGCLASS_XMM)
            vectors_.at(ZydisRegisterGetId(x.reg.value)) = data;
        else if (x.type == ZYDIS_OPERAND_TYPE_MEMORY) {
            const auto at = address(x);
            store(at, 64, data[0]);
            store(at + 8, 64, data[1]);
        } else
            throw fault{emulation_status::invalid_operand};
        return;
    }
    case ZYDIS_MNEMONIC_MOVQ: {
        const auto vector_value = [](const ZydisDecodedOperand& operand) {
            return operand.type == ZYDIS_OPERAND_TYPE_REGISTER &&
                ZydisRegisterGetClass(operand.reg.value) == ZYDIS_REGCLASS_XMM;
        };
        const auto scalar = [](const ZydisDecodedOperand& operand) {
            return operand.size == 64 &&
                (operand.type == ZYDIS_OPERAND_TYPE_MEMORY ||
                    (operand.type == ZYDIS_OPERAND_TYPE_REGISTER &&
                        ZydisRegisterGetClass(operand.reg.value) == ZYDIS_REGCLASS_GPR64));
        };
        if (i.operand_count_visible != 2 || (!vector_value(x) && !vector_value(y)) ||
            (!vector_value(x) && !scalar(x)) || (!vector_value(y) && !scalar(y)))
            throw fault{emulation_status::invalid_operand};
        const auto data = y.type == ZYDIS_OPERAND_TYPE_MEMORY ? load(address(y), 64) : value(y);
        if (vector_value(x))
            vectors_.at(ZydisRegisterGetId(x.reg.value)) = {data, 0};
        else
            write(x, data);
        return;
    }
    case ZYDIS_MNEMONIC_MOVSD: {
        const auto vector_value = [](const ZydisDecodedOperand& operand) {
            return operand.type == ZYDIS_OPERAND_TYPE_REGISTER &&
                ZydisRegisterGetClass(operand.reg.value) == ZYDIS_REGCLASS_XMM;
        };
        const auto scalar_memory = [](const ZydisDecodedOperand& operand) {
            return operand.type == ZYDIS_OPERAND_TYPE_MEMORY && operand.size == 64;
        };
        if (i.operand_count_visible != 2)
            throw fault{emulation_status::unsupported};
        if ((!vector_value(x) && !scalar_memory(x)) || (!vector_value(y) && !scalar_memory(y)) ||
            (!vector_value(x) && !vector_value(y)))
            throw fault{emulation_status::invalid_operand};
        const auto data =
            vector_value(y) ? vectors_.at(ZydisRegisterGetId(y.reg.value))[0] : load(address(y), 64);
        if (vector_value(x)) {
            auto& destination = vectors_.at(ZydisRegisterGetId(x.reg.value));
            destination[0] = data;
            if (scalar_memory(y))
                destination[1] = 0;
        } else {
            store(address(x), 64, data);
        }
        return;
    }
    case ZYDIS_MNEMONIC_MOVD:
    case ZYDIS_MNEMONIC_MOVSS: {
        const auto data = y.type == ZYDIS_OPERAND_TYPE_MEMORY ? load(address(y), 32) : value(y) & 0xffffffffu;
        if (x.type == ZYDIS_OPERAND_TYPE_MEMORY)
            store(address(x), 32, data);
        else if (ZydisRegisterGetClass(x.reg.value) == ZYDIS_REGCLASS_XMM) {
            auto& v = vectors_.at(ZydisRegisterGetId(x.reg.value));
            if (m == ZYDIS_MNEMONIC_MOVD || y.type == ZYDIS_OPERAND_TYPE_MEMORY)
                v = {};
            v[0] = (v[0] & 0xffffffff00000000ull) | data;
        } else
            write(x, data);
        return;
    }
    case ZYDIS_MNEMONIC_CVTSI2SS: {
        const auto integer = value(y);
        const float f = y.size == 64 ? static_cast<float>(static_cast<std::int64_t>(integer))
                                     : static_cast<float>(static_cast<std::int32_t>(integer));
        auto& v = vectors_.at(ZydisRegisterGetId(x.reg.value))[0];
        v = (v & 0xffffffff00000000ull) | std::bit_cast<std::uint32_t>(f);
        return;
    }
    case ZYDIS_MNEMONIC_CVTTSS2SI: {
        const float f = std::bit_cast<float>(static_cast<std::uint32_t>(
            y.type == ZYDIS_OPERAND_TYPE_MEMORY ? load(address(y), 32) : value(y)));
        const double limit = x.size == 64 ? 9223372036854775808.0 : 2147483648.0;
        write(x,
            !std::isfinite(f) || f < -limit || f >= limit
                ? (x.size == 64 ? 0x8000000000000000ull : 0x80000000ull)
                : static_cast<std::uint64_t>(static_cast<std::int64_t>(std::trunc(f))));
        return;
    }
    case ZYDIS_MNEMONIC_MULSS:
    case ZYDIS_MNEMONIC_DIVSS:
    case ZYDIS_MNEMONIC_ADDSS:
    case ZYDIS_MNEMONIC_SUBSS: {
        const float a = std::bit_cast<float>(static_cast<std::uint32_t>(value(x)));
        const float b = std::bit_cast<float>(static_cast<std::uint32_t>(
            y.type == ZYDIS_OPERAND_TYPE_MEMORY ? load(address(y), 32) : value(y)));
        const float f = m == ZYDIS_MNEMONIC_MULSS ? a * b
            : m == ZYDIS_MNEMONIC_DIVSS           ? a / b
            : m == ZYDIS_MNEMONIC_ADDSS           ? a + b
                                                  : a - b;
        auto& v = vectors_.at(ZydisRegisterGetId(x.reg.value))[0];
        v = (v & 0xffffffff00000000ull) | std::bit_cast<std::uint32_t>(f);
        return;
    }
    case ZYDIS_MNEMONIC_NOP:
    case ZYDIS_MNEMONIC_PAUSE:
        return;
    case ZYDIS_MNEMONIC_MOV:
        write(x, value(y));
        return;
    case ZYDIS_MNEMONIC_MOVZX:
        write(x, value(y) & mask(y.size));
        return;
    case ZYDIS_MNEMONIC_MOVSX:
    case ZYDIS_MNEMONIC_MOVSXD: {
        const auto source = value(y), sign = 1ull << (y.size - 1);
        write(x, (source ^ sign) - sign);
        return;
    }
    case ZYDIS_MNEMONIC_LEA:
        write(x, address(y, false));
        return;
    case ZYDIS_MNEMONIC_PUSH: {
        const auto source = value(x), sp = get(ZYDIS_REGISTER_RSP) - (x.size == 16 ? 2 : 8);
        store(sp, x.size == 16 ? 16 : 64, source);
        set(ZYDIS_REGISTER_RSP, sp);
        return;
    }
    case ZYDIS_MNEMONIC_POP: {
        const auto sp = get(ZYDIS_REGISTER_RSP);
        const unsigned bits_value = x.size == 16 ? 16 : 64;
        const auto source = load(sp, bits_value);
        set(ZYDIS_REGISTER_RSP, sp + bits_value / 8);
        write(x, source);
        return;
    }
    case ZYDIS_MNEMONIC_JMP:
        ip_ = value(x);
        return;
    case ZYDIS_MNEMONIC_CALL: {
        const auto target_value = value(x), sp = get(ZYDIS_REGISTER_RSP) - 8;
        store(sp, 64, next_);
        set(ZYDIS_REGISTER_RSP, sp);
        ip_ = target_value;
        return;
    }
    case ZYDIS_MNEMONIC_RET: {
        const auto sp = get(ZYDIS_REGISTER_RSP);
        ip_ = load(sp, 64);
        set(ZYDIS_REGISTER_RSP, sp + 8 + (i.operand_count_visible ? value(x) : 0));
        return;
    }
    case ZYDIS_MNEMONIC_JRCXZ:
        if (!get(ZYDIS_REGISTER_RCX))
            ip_ = value(x);
        return;
    case ZYDIS_MNEMONIC_JECXZ:
        if (!get(ZYDIS_REGISTER_ECX))
            ip_ = value(x);
        return;
    case ZYDIS_MNEMONIC_NOT:
        write(x, ~value(x));
        return;
    case ZYDIS_MNEMONIC_NEG: {
        const auto a = value(x);
        arithmetic(0, a, 0 - a, x.size, true);
        write(x, 0 - a);
        return;
    }
    case ZYDIS_MNEMONIC_INC:
    case ZYDIS_MNEMONIC_DEC: {
        const auto a = value(x), result_value = m == ZYDIS_MNEMONIC_INC ? a + 1 : a - 1, carry = flags_ & cf;
        arithmetic(a, 1, result_value, x.size, m == ZYDIS_MNEMONIC_DEC);
        flags_ = (flags_ & ~cf) | carry;
        write(x, result_value);
        return;
    }
    case ZYDIS_MNEMONIC_ADC:
    case ZYDIS_MNEMONIC_SBB: {
        const auto a = value(x) & mask(x.size), b = value(y) & mask(x.size), carry = flags_ & cf;
        const bool subtract = m == ZYDIS_MNEMONIC_SBB;
        const auto result_value = (subtract ? a - b - carry : a + b + carry) & mask(x.size);
        arithmetic(a, b, result_value, x.size, subtract);
        flags_ &= ~cf;
        if (subtract ? a < b || (carry && a == b) : result_value < a || (carry && result_value == a))
            flags_ |= cf;
        write(x, result_value);
        return;
    }
    case ZYDIS_MNEMONIC_ADD:
    case ZYDIS_MNEMONIC_SUB:
    case ZYDIS_MNEMONIC_CMP: {
        const auto a = value(x), b = value(y);
        const bool subtract = m != ZYDIS_MNEMONIC_ADD;
        const auto result_value = subtract ? a - b : a + b;
        arithmetic(a, b, result_value, x.size, subtract);
        if (m != ZYDIS_MNEMONIC_CMP)
            write(x, result_value);
        return;
    }
    case ZYDIS_MNEMONIC_AND:
    case ZYDIS_MNEMONIC_OR:
    case ZYDIS_MNEMONIC_XOR:
    case ZYDIS_MNEMONIC_TEST: {
        const auto a = value(x), b = value(y),
                   result_value = m == ZYDIS_MNEMONIC_OR ? a | b
            : m == ZYDIS_MNEMONIC_XOR                    ? a ^ b
                                                         : a & b;
        logical(result_value, x.size);
        if (m != ZYDIS_MNEMONIC_TEST)
            write(x, result_value);
        return;
    }
    case ZYDIS_MNEMONIC_IMUL: {
        if (i.operand_count_visible == 1) {
            const unsigned bits_value = x.size;
            if (bits_value != 8 && bits_value != 16 && bits_value != 32 && bits_value != 64)
                throw fault{emulation_status::invalid_operand};
            const auto limit = mask(bits_value), sign = 1ull << (bits_value - 1);
            const auto a = get(ZYDIS_REGISTER_RAX) & limit, b = value(x) & limit;
            const auto signed_value = [&](std::uint64_t value_data) {
                return std::bit_cast<std::int64_t>((value_data ^ sign) - sign);
            };
            std::uint64_t low{}, high{};
            if (bits_value == 64) {
                std::int64_t upper{};
                low = std::bit_cast<std::uint64_t>(_mul128(signed_value(a), signed_value(b), &upper));
                high = std::bit_cast<std::uint64_t>(upper);
                set(ZYDIS_REGISTER_RAX, low);
                set(ZYDIS_REGISTER_RDX, high);
            } else {
                const auto product = std::bit_cast<std::uint64_t>(signed_value(a) * signed_value(b));
                low = product & limit;
                high = (product >> bits_value) & limit;
                if (bits_value == 8)
                    set(ZYDIS_REGISTER_AX, product);
                else {
                    set(bits_value == 16 ? ZYDIS_REGISTER_AX : ZYDIS_REGISTER_EAX, low);
                    set(bits_value == 16 ? ZYDIS_REGISTER_DX : ZYDIS_REGISTER_EDX, high);
                }
            }
            flags_ &= ~(cf | of);
            if (high != ((low & sign) ? limit : 0))
                flags_ |= cf | of;
            return;
        }
        if (i.operand_count_visible < 2 || i.operand_count_visible > 3)
            throw fault{emulation_status::unsupported};
        const auto a = i.operand_count_visible == 3 ? value(y) : value(x),
                   b = i.operand_count_visible == 3 ? value(d.operands[2]) : value(y);
        write(x, a * b);
        flags_ &= ~(cf | of);
        const auto sign = 1ull << (x.size - 1), limit = mask(x.size);
        const auto magnitude = [&](std::uint64_t v) {
            v &= limit;
            return (v & sign) ? ((~v + 1) & limit) : v;
        };
        const auto aa = magnitude(a), bb = magnitude(b), maximum = ((a ^ b) & sign) ? sign : sign - 1;
        if (bb && aa > maximum / bb)
            flags_ |= cf | of;
        return;
    }
    case ZYDIS_MNEMONIC_SHLD:
    case ZYDIS_MNEMONIC_SHRD: {
        const auto& z = d.operands[2];
        const unsigned bits_value = x.size;
        const auto scalar = [&](const ZydisDecodedOperand& operand) {
            return operand.type == ZYDIS_OPERAND_TYPE_REGISTER && operand.size == bits_value &&
                ZydisRegisterGetClass(operand.reg.value) ==
                (bits_value == 64          ? ZYDIS_REGCLASS_GPR64
                        : bits_value == 32 ? ZYDIS_REGCLASS_GPR32
                                           : ZYDIS_REGCLASS_GPR16);
        };
        if (i.operand_count_visible != 3 || (bits_value != 16 && bits_value != 32 && bits_value != 64) ||
            (!scalar(x) && x.type != ZYDIS_OPERAND_TYPE_MEMORY) || !scalar(y) || z.size != 8 ||
            (z.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && z.imm.is_relative) ||
            (z.type != ZYDIS_OPERAND_TYPE_IMMEDIATE &&
                (z.type != ZYDIS_OPERAND_TYPE_REGISTER || z.reg.value != ZYDIS_REGISTER_CL)))
            throw fault{emulation_status::invalid_operand};
        const auto count_value = static_cast<unsigned>(value(z) & (bits_value == 64 ? 63 : 31));
        if (count_value > bits_value)
            throw fault{emulation_status::unsupported};
        const auto limit = mask(bits_value), a = value(x) & limit;
        if (!count_value) {
            if (x.type == ZYDIS_OPERAND_TYPE_REGISTER && bits_value == 32)
                write(x, a);
            return;
        }
        const auto b = value(y) & limit;
        const bool left = m == ZYDIS_MNEMONIC_SHLD;
        const auto result_value = count_value == bits_value ? b
            : left ? ((a << count_value) | (b >> (bits_value - count_value))) & limit
                   : ((a >> count_value) | (b << (bits_value - count_value))) & limit;
        const auto carry = left ? (a >> (bits_value - count_value)) & 1 : (a >> (count_value - 1)) & 1;
        logical(result_value, bits_value);
        if (carry)
            flags_ |= cf;
        if (count_value == 1 && ((a ^ result_value) & (1ull << (bits_value - 1))))
            flags_ |= of;
        write(x, result_value);
        return;
    }
    case ZYDIS_MNEMONIC_SHL:
    case ZYDIS_MNEMONIC_SHR:
    case ZYDIS_MNEMONIC_SAR:
    case ZYDIS_MNEMONIC_ROL:
    case ZYDIS_MNEMONIC_ROR: {
        const unsigned bits_value = x.size;
        const auto limit = mask(bits_value), a = value(x) & limit;
        const auto masked_count = static_cast<unsigned>(value(y) & (bits_value == 64 ? 63 : 31));
        auto count_value = masked_count;
        const bool rotate = m == ZYDIS_MNEMONIC_ROL || m == ZYDIS_MNEMONIC_ROR;
        if (rotate)
            count_value %= bits_value;
        if (!count_value) {
            if (rotate && masked_count) {
                write(x, a);
                const auto carry = m == ZYDIS_MNEMONIC_ROL ? a & 1 : (a >> (bits_value - 1)) & 1;
                flags_ = (flags_ & ~cf) | (carry ? cf : 0);
            } else if (x.type == ZYDIS_OPERAND_TYPE_REGISTER && bits_value == 32)
                write(x, a);
            return;
        }
        std::uint64_t result_value{}, carry{};
        const auto old_sign = (a >> (bits_value - 1)) & 1;
        if (rotate) {
            result_value = m == ZYDIS_MNEMONIC_ROL ? ((a << count_value) | (a >> (bits_value - count_value)))
                                                   : ((a >> count_value) | (a << (bits_value - count_value)));
            result_value &= limit;
            carry = m == ZYDIS_MNEMONIC_ROL ? result_value & 1 : (result_value >> (bits_value - 1)) & 1;
            flags_ = (flags_ & ~cf) | (carry ? cf : 0);
        } else {
            if (count_value > bits_value)
                throw fault{emulation_status::unsupported};
            if (m == ZYDIS_MNEMONIC_SHL) {
                carry = (a >> (bits_value - count_value)) & 1;
                result_value = count_value == 64 ? 0 : a << count_value;
            } else {
                carry = (a >> (count_value - 1)) & 1;
                result_value = count_value == 64 ? 0 : a >> count_value;
                if (m == ZYDIS_MNEMONIC_SAR && old_sign)
                    result_value |= limit ^ (count_value >= bits_value ? 0 : mask(bits_value - count_value));
            }
            logical(result_value, bits_value);
            if (carry)
                flags_ |= cf;
        }
        if (count_value == 1) {
            bool overflow{};
            if (m == ZYDIS_MNEMONIC_SHR)
                overflow = old_sign != 0;
            else if (m == ZYDIS_MNEMONIC_ROL || m == ZYDIS_MNEMONIC_SHL)
                overflow = ((result_value >> (bits_value - 1)) & 1) != carry;
            else if (m == ZYDIS_MNEMONIC_ROR)
                overflow =
                    (((result_value >> (bits_value - 1)) ^ (result_value >> (bits_value - 2))) & 1) != 0;
            flags_ = (flags_ & ~of) | (overflow ? of : 0);
        }
        write(x, result_value);
        return;
    }
    default:
        if (triton_step(d))
            return;
        throw fault{emulation_status::unsupported};
    }
}

bool emulator::triton_step(const decoded& instruction) {
    switch (instruction.instruction.mnemonic) {
    case ZYDIS_MNEMONIC_PADDB:
    case ZYDIS_MNEMONIC_PADDW:
    case ZYDIS_MNEMONIC_PADDD:
    case ZYDIS_MNEMONIC_PADDQ:
    case ZYDIS_MNEMONIC_PSUBB:
    case ZYDIS_MNEMONIC_PSUBW:
    case ZYDIS_MNEMONIC_PSUBD:
    case ZYDIS_MNEMONIC_PSUBQ:
    case ZYDIS_MNEMONIC_PCMPEQB:
    case ZYDIS_MNEMONIC_PCMPEQW:
    case ZYDIS_MNEMONIC_PCMPEQD:
    case ZYDIS_MNEMONIC_PCMPEQQ:
    case ZYDIS_MNEMONIC_PMOVMSKB:
    case ZYDIS_MNEMONIC_BSWAP:
    case ZYDIS_MNEMONIC_POPCNT:
    case ZYDIS_MNEMONIC_LZCNT:
    case ZYDIS_MNEMONIC_TZCNT:
        break;
    default:
        return false;
    }
    ZydisDecoder decoder{};
    ZydisDecodedInstruction native{};
    std::array<ZydisDecodedOperand, ZYDIS_MAX_OPERAND_COUNT> operands{};
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(
            &decoder, instruction.bytes.data(), instruction.instruction.length, &native, operands.data())) ||
        std::memcmp(&native, &instruction.instruction, sizeof(native)) != 0 ||
        std::memcmp(operands.data(), instruction.operands.data(), sizeof(operands)) != 0)
        return false;
    std::vector<sem::block> reads;
    for (unsigned i = 0; i < native.operand_count; ++i) {
        const auto& operand = operands[i];
        if (operand.type != ZYDIS_OPERAND_TYPE_MEMORY)
            continue;
        if ((operand.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) ||
            !(operand.actions & ZYDIS_OPERAND_ACTION_MASK_READ) || !operand.size || operand.size > 128 ||
            operand.size % 8 || operand.mem.segment == ZYDIS_REGISTER_FS)
            return false;
        const auto at = address(operand);
        sem::block block{at, std::vector<std::byte>(operand.size / 8)};
        for (std::size_t offset = 0; offset < block.bytes.size();) {
            const auto count = std::min<std::size_t>(8, block.bytes.size() - offset);
            const auto value = load(at + offset, static_cast<unsigned>(count * 8));
            std::memcpy(block.bytes.data() + offset, &value, count);
            offset += count;
        }
        reads.push_back(std::move(block));
    }
    auto state = snapshot();
    if (!sem::step(state, std::span(instruction.bytes).first(native.length), instruction.address, reads))
        return false;
    registers_ = state.registers;
    vectors_ = state.vectors;
    flags_ = state.flags;
    return true;
}

emulation_result emulator::run(const program& code, std::size_t budget) {
    if (code.linear) {
        emulation_result result_value{emulation_status::complete, code.begin, ZYDIS_MNEMONIC_INVALID, 0};
        for (const auto& instruction_value : code.instructions) {
            result_value.address = instruction_value.address;
            result_value.mnemonic = instruction_value.instruction.mnemonic;
            if (result_value.steps >= budget) {
                result_value.status = emulation_status::budget_exhausted;
                return result_value;
            }
            next_ = instruction_value.address + instruction_value.instruction.length;
            current_ = instruction_value.address;
            ip_ = next_;
            address_width_ = instruction_value.instruction.address_width;
            ++result_value.steps;
            try {
                step(instruction_value);
            } catch (fault f) {
                result_value.status = f.status;
                return result_value;
            }
            if (ip_ != next_) {
                result_value.status = emulation_status::control_flow;
                return result_value;
            }
        }
        result_value.address = code.end;
        return result_value;
    }
    ip_ = code.begin;
    emulation_result result_value{emulation_status::complete, ip_, ZYDIS_MNEMONIC_INVALID, 0};
    while (ip_ != code.end) {
        result_value.address = ip_;
        if (result_value.steps >= budget) {
            result_value.status = emulation_status::budget_exhausted;
            return result_value;
        }
        const auto found = std::lower_bound(code.instructions.begin(), code.instructions.end(), ip_,
            [](const decoded& d, std::uint64_t at) { return d.address < at; });
        if (found == code.instructions.end() || found->address != ip_) {
            result_value.status = emulation_status::control_flow;
            return result_value;
        }
        result_value.mnemonic = found->instruction.mnemonic;
        next_ = ip_ + found->instruction.length;
        current_ = found->address;
        ip_ = next_;
        address_width_ = found->instruction.address_width;
        ++result_value.steps;
        try {
            step(*found);
        } catch (fault f) {
            result_value.status = f.status;
            return result_value;
        }
        if (code.sparse && ip_ != code.end) {
            const auto next = std::lower_bound(code.instructions.begin(), code.instructions.end(), ip_,
                [](const decoded& d, std::uint64_t at) { return d.address < at; });
            if (next == code.instructions.end()) {
                result_value.status = emulation_status::control_flow;
                return result_value;
            }
            ip_ = next->address;
        }
    }
    result_value.address = ip_;
    return result_value;
}
}
