#include "lifus/unlockall/qb/Image.h"
#include "lifus/unlockall/qb/Fault.h"
#include "lifus/unlockall/mem/Triton.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace uwu::sdk::locators {
std::span<const std::byte> code_image::bytes(std::uint64_t address, std::size_t size) const noexcept {
    for (const auto& region : {std::pair{base_ + code_rva_, code_}, std::pair{base_ + data_rva_, data_}})
        if (address >= region.first && address - region.first <= region.second.size() &&
            size <= region.second.size() - static_cast<std::size_t>(address - region.first))
            return region.second.subspan(static_cast<std::size_t>(address - region.first), size);
    return {};
}

std::optional<std::uint64_t> code_image::value(const expression& expression) const {
    if (expression.inputs)
        return {};
    memory unattached;
    emulator cpu(unattached);
    cpu.reset();
    if (!code_.empty())
        cpu.input(base_ + code_rva_, code_);
    if (!data_.empty())
        cpu.input(base_ + data_rva_, data_);
    return cpu.run(expression.code).complete() ? std::optional(cpu.get(expression.output)) : std::nullopt;
}

namespace {
std::optional<std::uint64_t> target(const emulator::decoded& instruction) {
    const auto category = instruction.instruction.meta.category;
    if (category != ZYDIS_CATEGORY_CALL && category != ZYDIS_CATEGORY_UNCOND_BR &&
        category != ZYDIS_CATEGORY_COND_BR)
        return {};
    const auto& operand = instruction.operands[0];
    if (operand.type != ZYDIS_OPERAND_TYPE_IMMEDIATE || !operand.imm.is_relative)
        return {};
    return instruction.address + instruction.instruction.length + operand.imm.value.s;
}
}

code_image::code_image(std::span<const std::byte> code, std::uint32_t code_rva, std::uint64_t base,
    std::span<const function_record> functions, std::span<const std::byte> data, std::uint32_t data_rva)
    : code_(code), data_(data), functions_(functions), base_(base), code_rva_(code_rva), data_rva_(data_rva) {
    const auto room = std::numeric_limits<std::uint64_t>::max() - base;
    if (code_rva > room || code.size() > room - code_rva || data_rva > room ||
        data.size() > room - data_rva || code.size() > 512ull * 1024 * 1024 ||
        data.size() > 32ull * 1024 * 1024)
        throw std::invalid_argument("image bounds");
    if (!code.empty() && !data.empty() && code_rva < data_rva + data.size() &&
        data_rva < code_rva + code.size())
        throw std::invalid_argument("overlapping image regions");
    std::uint32_t previous{}, previous_end{};
    bool first = true;
    for (const auto& value : functions_) {
        if (value.begin >= value.end || (!first && value.begin <= previous))
            throw std::invalid_argument("function bounds");
        previous = value.begin;
        first = false;
        if (value.begin >= code_rva && value.end - code_rva <= code.size()) {
            if (value.begin < previous_end)
                throw std::invalid_argument("overlapping code functions");
            previous_end = value.end;
        }
    }
    for (std::size_t offset = 0; offset + 8 <= data_.size(); offset += 8) {
        std::uint64_t entry{};
        std::memcpy(&entry, data_.data() + offset, 8);
        if (entry >= base_ + code_rva_ && entry - base_ - code_rva_ < code_.size()) {
            entries_.insert(entry);
            if (entries_.size() > 262144)
                throw std::invalid_argument("image reference budget");
        }
    }
}

std::optional<function_record> code_image::function(std::uint64_t address) const {
    if (address < base_ || address - base_ > std::numeric_limits<std::uint32_t>::max())
        return {};
    const auto rva = address - base_;
    auto found = std::upper_bound(functions_.begin(), functions_.end(), rva,
        [](auto at, const auto& value) { return at < value.begin; });
    if (found == functions_.begin())
        return {};
    --found;
    return rva < found->end ? std::optional(*found) : std::nullopt;
}

bool code_image::terminal_fault(const function_record& record) const {
    const auto unwind = bytes(base_ + record.unwind, 1);
    if (unwind.size() != 1 || std::to_integer<unsigned>(unwind.front()) != 1)
        return false;
    const auto raw = bytes(base_ + record.begin, record.end - record.begin);
    const auto code = emulator::decode(raw, base_ + record.begin);
    if (!code || code->instructions.empty() || code->end != base_ + record.end)
        return false;
    const auto faults = normal_execution_faults(*code);
    if (!faults.opcode_binding)
        return false;
    const auto found = std::find_if(faults.faults.begin(), faults.faults.end(),
        [&](const auto& fault) { return fault.instruction == code->instructions.back().address; });
    if (found == faults.faults.end())
        return false;
    const auto branches = jumps(*code);
    for (const auto& branch : branches) {
        if (branch.targets.empty())
            return false;
        for (const auto destination : branch.targets)
            if (destination > found->block && destination <= found->instruction)
                return false;
    }
    return true;
}

std::optional<emulator::program> code_image::body(std::uint64_t entry, const code_limits& limits) const {
    const auto begin = base_ + code_rva_;
    if (entry < begin || entry - begin >= code_.size() || !limits.function_bytes || !limits.leaf_bytes)
        return {};
    const auto record = function(entry);
    auto length = code_.size() - static_cast<std::size_t>(entry - begin);
    if (record) {
        if (entry != base_ + record->begin || record->end - record->begin > limits.function_bytes ||
            record->begin < code_rva_ || record->end - code_rva_ > code_.size())
            return {};
        length = std::min(length, static_cast<std::size_t>(record->end - record->begin));
    } else {
        const auto next = std::lower_bound(functions_.begin(), functions_.end(), entry - base_,
            [](const auto& value, auto at) { return value.begin < at; });
        if (next != functions_.end())
            length = std::min(length, static_cast<std::size_t>(base_ + next->begin - entry));
        length = std::min(length, limits.leaf_bytes);
    }
    const auto end = entry + length;
    std::map<std::uint64_t, emulator::decoded> instructions;
    std::vector<std::uint64_t> pending{entry};
    ZydisDecoder decoder{};
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    while (!pending.empty()) {
        auto at = pending.back();
        pending.pop_back();
        std::vector<emulator::decoded> block;
        while (at >= entry && at < end) {
            const auto next_known = instructions.lower_bound(at);
            if (next_known != instructions.end() && next_known->first == at)
                break;
            if (next_known != instructions.begin()) {
                const auto& prior = std::prev(next_known)->second;
                if (prior.address + prior.instruction.length > at)
                    return {};
            }
            if (instructions.size() >= limits.instructions)
                return {};
            emulator::decoded decoded{};
            decoded.address = at;
            const auto available = std::min<std::size_t>(15, end - at);
            std::copy_n(code_.data() + (at - begin), available, decoded.bytes.data());
            if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, decoded.bytes.data(), available,
                    &decoded.instruction, decoded.operands.data())))
                return {};
            const auto next = at + decoded.instruction.length;
            if (next_known != instructions.end() && next_known->first < next)
                return {};
            instructions.emplace(at, decoded);
            block.push_back(decoded);
            const auto category = decoded.instruction.meta.category;
            if (category == ZYDIS_CATEGORY_RET || category == ZYDIS_CATEGORY_INTERRUPT)
                break;
            if (category == ZYDIS_CATEGORY_CALL) {
                const auto called = target(decoded);
                if (called && no_return(*called))
                    break;
            }
            if (category == ZYDIS_CATEGORY_COND_BR || category == ZYDIS_CATEGORY_UNCOND_BR) {
                if (const auto destination = target(decoded);
                    destination && *destination >= entry && *destination < end)
                    pending.push_back(*destination);
                if (category == ZYDIS_CATEGORY_UNCOND_BR && !target(decoded)) {
                    if (const auto branches = resolve(block))
                        for (const auto destination : branches->targets)
                            if (destination >= entry && destination < end)
                                pending.push_back(destination);
                }
                if (category == ZYDIS_CATEGORY_UNCOND_BR)
                    break;
            }
            if (category == ZYDIS_CATEGORY_CALL || category == ZYDIS_CATEGORY_COND_BR)
                block.clear();
            if (next == end && record && decoded.operands[0].type == ZYDIS_OPERAND_TYPE_MEMORY &&
                (decoded.operands[0].actions & ZYDIS_OPERAND_ACTION_WRITE) && terminal_fault(*record))
                break;
            if (next == end)
                return {};
            at = next;
        }
    }
    if (instructions.empty())
        return {};
    emulator::program result;
    result.begin = entry;
    const auto& last = instructions.rbegin()->second;
    result.end = record ? end : last.address + last.instruction.length;
    for (auto& [address, instruction] : instructions) {
        static_cast<void>(address);
        result.instructions.push_back(std::move(instruction));
    }
    return result;
}

std::vector<code_jump> code_image::jumps(const emulator::program& program) const {
    std::vector<code_jump> result;
    const auto& instructions = program.instructions;
    for (std::size_t i = 0; i < instructions.size(); ++i) {
        const auto& instruction = instructions[i];
        if (instruction.instruction.meta.category != ZYDIS_CATEGORY_UNCOND_BR || target(instruction))
            continue;
        auto first = i;
        while (first && i - first < 127) {
            const auto& prior = instructions[first - 1];
            const auto category = prior.instruction.meta.category;
            if (prior.address + prior.instruction.length != instructions[first].address ||
                category == ZYDIS_CATEGORY_CALL || category == ZYDIS_CATEGORY_RET ||
                category == ZYDIS_CATEGORY_COND_BR || category == ZYDIS_CATEGORY_UNCOND_BR ||
                category == ZYDIS_CATEGORY_INTERRUPT)
                break;
            --first;
        }
        code_jump jump{instructions[first].address, instruction.address, {}, 0};
        if (const auto resolved = resolve(std::span(instructions).subspan(first, i - first + 1))) {
            jump.targets = resolved->targets;
            jump.cases = resolved->cases;
        }
        result.push_back(std::move(jump));
    }
    std::vector<std::uint64_t> destinations;
    for (const auto& instruction : instructions) {
        const auto category = instruction.instruction.meta.category;
        if (category == ZYDIS_CATEGORY_COND_BR || category == ZYDIS_CATEGORY_UNCOND_BR)
            if (const auto destination = target(instruction))
                destinations.push_back(*destination);
    }
    for (const auto& jump : result)
        destinations.insert(destinations.end(), jump.targets.begin(), jump.targets.end());
    for (auto& jump : result)
        if (std::any_of(destinations.begin(), destinations.end(),
                [&](auto target) { return target > jump.begin && target <= jump.instruction; })) {
            jump.targets.clear();
            jump.cases = 0;
        }
    return result;
}

std::optional<code_jump> code_image::resolve(std::span<const emulator::decoded> block) const {
    if (block.empty() || block.size() > 128)
        return {};
    for (const auto& cached : branches_)
        if (cached.instructions.size() == block.size() &&
            std::equal(
                block.begin(), block.end(), cached.instructions.begin(), [](const auto& a, const auto& b) {
                    return a.address == b.address && a.bytes == b.bytes &&
                        std::memcmp(&a.instruction, &b.instruction, sizeof(a.instruction)) == 0 &&
                        std::memcmp(a.operands.data(), b.operands.data(), sizeof(a.operands)) == 0;
                }))
            return cached.result;
    const std::array regions{sem::region{base_ + code_rva_, code_}, sem::region{base_ + data_rva_, data_}};
    std::optional<code_jump> result;
    if (const auto resolved = sem::targets(block, regions))
        result = code_jump{block.front().address, block.back().address, resolved->targets, resolved->cases};
    if (branches_.size() < 128)
        branches_.push_back({{block.begin(), block.end()}, result});
    return result;
}

code_references code_image::references(
    std::string_view literal, std::stop_token stop, std::size_t limit) const {
    code_references result;
    if (literal.empty() || literal.size() > 1024 || !limit || limit > 4096)
        return {code_status::invalid, {}, 0};
    if (stop.stop_requested())
        return {code_status::cancelled, {}, 0};
    std::set<std::uint64_t> labels;
    const auto bytes = std::as_bytes(std::span(literal.data(), literal.size()));
    for (std::size_t offset = 0; offset < data_.size();) {
        const auto found = std::search(data_.begin() + offset, data_.end(), bytes.begin(), bytes.end());
        if (found == data_.end())
            break;
        offset = static_cast<std::size_t>(found - data_.begin());
        if (offset + bytes.size() < data_.size() && data_[offset + bytes.size()] == std::byte{})
            labels.insert(base_ + data_rva_ + offset);
        offset += bytes.size();
    }
    if (labels.empty())
        return result;
    std::set<std::uint32_t> candidates;
    for (std::size_t offset = 0; offset + 7 <= code_.size(); ++offset) {
        if ((offset & 0xffff) == 0 && stop.stop_requested())
            return {code_status::cancelled, {}, 0};
        const auto rex = std::to_integer<unsigned>(code_[offset]);
        const auto modrm = std::to_integer<unsigned>(code_[offset + 2]);
        const auto opcode = code_[offset + 1];
        if ((rex & 0xf8) != 0x48 || (opcode != std::byte{0x8d} && opcode != std::byte{0x8b}) ||
            (modrm & 0xc7) != 5)
            continue;
        std::int32_t displacement{};
        std::memcpy(&displacement, code_.data() + offset + 3, 4);
        const auto address = base_ + code_rva_ + offset;
        auto value = address + 7 + displacement;
        if (opcode == std::byte{0x8b}) {
            if (value < base_ + data_rva_ || value - base_ - data_rva_ > data_.size() ||
                data_.size() - static_cast<std::size_t>(value - base_ - data_rva_) < 8)
                continue;
            std::memcpy(&value, data_.data() + (value - base_ - data_rva_), 8);
        }
        if (!labels.contains(value))
            continue;
        if (const auto record = function(address))
            candidates.insert(record->begin);
        else {
            bool found{};
            const auto lower = address - std::min<std::uint64_t>(address - base_ - code_rva_, 1024);
            for (auto entry = entries_.lower_bound(lower); entry != entries_.end() && *entry <= address;
                ++entry) {
                if (function(*entry))
                    continue;
                const auto program = body(*entry);
                if (!program || address >= program->end)
                    continue;
                if (std::any_of(program->instructions.begin(), program->instructions.end(),
                        [&](const auto& instruction) { return instruction.address == address; })) {
                    candidates.insert(static_cast<std::uint32_t>(*entry - base_));
                    found = true;
                }
            }
            if (!found) {
                ++result.rejected_functions;
                result.status = code_status::unsupported;
            }
        }
    }
    for (const auto begin : candidates) {
        if (stop.stop_requested())
            return {code_status::cancelled, {}, 0};
        const auto program = body(base_ + begin);
        if (!program) {
            ++result.rejected_functions;
            result.status = code_status::unsupported;
            continue;
        }
        const auto branches = jumps(*program);
        if (std::any_of(
                branches.begin(), branches.end(), [](const auto& jump) { return jump.targets.empty(); }))
            result.status = code_status::unsupported;
        for (const auto& instruction : program->instructions) {
            if ((instruction.instruction.mnemonic != ZYDIS_MNEMONIC_LEA &&
                    instruction.instruction.mnemonic != ZYDIS_MNEMONIC_MOV) ||
                instruction.operands[0].type != ZYDIS_OPERAND_TYPE_REGISTER ||
                instruction.operands[0].size != 64 ||
                instruction.operands[1].type != ZYDIS_OPERAND_TYPE_MEMORY ||
                instruction.operands[1].mem.base != ZYDIS_REGISTER_RIP ||
                instruction.operands[1].mem.index != ZYDIS_REGISTER_NONE)
                continue;
            auto address =
                instruction.address + instruction.instruction.length + instruction.operands[1].mem.disp.value;
            if (instruction.instruction.mnemonic == ZYDIS_MNEMONIC_MOV) {
                if (instruction.operands[1].size != 64 || address < base_ + data_rva_ ||
                    address - base_ - data_rva_ > data_.size() ||
                    data_.size() - static_cast<std::size_t>(address - base_ - data_rva_) < 8)
                    continue;
                std::memcpy(&address, data_.data() + (address - base_ - data_rva_), 8);
            }
            if (!labels.contains(address))
                continue;
            if (result.values.size() == limit) {
                result.status = code_status::budget;
                return result;
            }
            const auto owner =
                function(instruction.address)
                    .value_or(function_record{static_cast<std::uint32_t>(program->begin - base_),
                        static_cast<std::uint32_t>(program->end - base_), 0});
            result.values.push_back({address, instruction.address, owner, instruction.operands[0].reg.value});
        }
    }
    return result;
}

}
