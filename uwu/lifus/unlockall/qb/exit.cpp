#include "lifus/unlockall/qb/Image.h"
#include <algorithm>

namespace uwu::sdk::locators {
bool code_image::no_return(std::uint64_t entry) const {
    if (const auto found = exits_.find(entry); found != exits_.end())
        return found->second;
    const auto prove = [&]() {
        const auto record = function(entry);
        if (!record || base_ + record->begin != entry || record->begin < code_rva_ ||
            record->end - code_rva_ > code_.size() || record->end - record->begin > 4096)
            return false;
        const auto end = base_ + record->end;
        std::map<std::uint64_t, std::uint64_t> decoded;
        std::vector<std::uint64_t> pending{entry};
        ZydisDecoder decoder{};
        ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
        while (!pending.empty()) {
            const auto at = pending.back();
            pending.pop_back();
            if (at < entry || at >= end)
                return false;
            const auto found = decoded.lower_bound(at);
            if (found != decoded.end() && found->first == at)
                continue;
            if (decoded.size() >= 512 || (found != decoded.begin() && std::prev(found)->second > at))
                return false;
            ZydisDecodedInstruction instruction{};
            std::array<ZydisDecodedOperand, ZYDIS_MAX_OPERAND_COUNT> operands{};
            if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, code_.data() + (at - base_ - code_rva_),
                    std::min<std::uint64_t>(15, end - at), &instruction, operands.data())))
                return false;
            const auto next = at + instruction.length;
            if (found != decoded.end() && found->first < next)
                return false;
            decoded.emplace(at, next);
            const auto category = instruction.meta.category;
            if (category == ZYDIS_CATEGORY_RET || category == ZYDIS_CATEGORY_SYSCALL ||
                category == ZYDIS_CATEGORY_SYSRET)
                return false;
            if (instruction.mnemonic == ZYDIS_MNEMONIC_INT3 || instruction.mnemonic == ZYDIS_MNEMONIC_UD2) {
                if (next != end)
                    return false;
                continue;
            }
            if (category == ZYDIS_CATEGORY_INTERRUPT)
                return false;
            if (category == ZYDIS_CATEGORY_COND_BR || category == ZYDIS_CATEGORY_UNCOND_BR) {
                const auto& operand = operands[0];
                if (operand.type != ZYDIS_OPERAND_TYPE_IMMEDIATE || !operand.imm.is_relative)
                    return false;
                pending.push_back(next + operand.imm.value.s);
            }
            if (category != ZYDIS_CATEGORY_UNCOND_BR)
                pending.push_back(next);
        }
        return !decoded.empty();
    };
    const bool result = prove();
    if (exits_.size() < 4096)
        exits_.emplace(entry, result);
    return result;
}
}
