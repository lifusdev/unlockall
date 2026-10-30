#include "lifus/unlockall/unlock/Profile.h"
#include "lifus/unlockall/log/Log.h"
#include <algorithm>
#include <cstring>
#include <set>

namespace uwu::sdk {
namespace {
using namespace locators;
using program = emulator::program;

std::optional<std::uint64_t> call(const emulator::decoded& instruction) {
    const auto& operand = instruction.operands[0];
    if (instruction.instruction.meta.category != ZYDIS_CATEGORY_CALL ||
        operand.type != ZYDIS_OPERAND_TYPE_IMMEDIATE || !operand.imm.is_relative)
        return {};
    return instruction.address + instruction.instruction.length + operand.imm.value.s;
}

std::optional<std::size_t> index(const program& code, std::uint64_t address) {
    const auto found = std::lower_bound(code.instructions.begin(), code.instructions.end(), address,
        [](const auto& instruction, auto at) { return instruction.address < at; });
    return found != code.instructions.end() && found->address == address
        ? std::optional<std::size_t>(found - code.instructions.begin())
        : std::nullopt;
}

bool member(const ZydisDecodedOperand& operand, unsigned bits = 64) {
    return operand.type == ZYDIS_OPERAND_TYPE_MEMORY && operand.size == bits &&
        ZydisRegisterGetClass(operand.mem.base) == ZYDIS_REGCLASS_GPR64 &&
        operand.mem.index == ZYDIS_REGISTER_NONE && operand.mem.segment != ZYDIS_REGISTER_FS &&
        operand.mem.segment != ZYDIS_REGISTER_GS && operand.mem.disp.value >= 0 &&
        operand.mem.disp.value <= 0x100000;
}

std::optional<std::uint64_t> constant(
    const code_image& image, const program& code, std::size_t before, ZydisRegister reg) {
    const auto expression = slice(code, before, reg);
    return expression ? image.value(*expression) : std::nullopt;
}

bool table(const code_image& image, std::uint64_t address) {
    const auto bytes = image.bytes(address, 8);
    if (bytes.size() != 8)
        return false;
    std::uint64_t entry{};
    std::memcpy(&entry, bytes.data(), 8);
    const auto function = image.function(entry);
    return function && image.base() + function->begin == entry;
}

std::optional<std::uint64_t> type_table(const code_image& image, const program& code, bool receiver) {
    std::vector<std::pair<std::size_t, std::uint64_t>> stores;
    std::vector<std::size_t> assignments;
    std::vector<std::size_t> returns;
    for (std::size_t i = 0; i < code.instructions.size(); ++i) {
        const auto& instruction = code.instructions[i];
        if (instruction.instruction.meta.category == ZYDIS_CATEGORY_RET)
            returns.push_back(i);
        if (instruction.instruction.mnemonic != ZYDIS_MNEMONIC_MOV || !member(instruction.operands[0]) ||
            instruction.operands[0].mem.disp.value)
            continue;
        if (receiver && !image.same_value(code, 0, ZYDIS_REGISTER_RCX, i, instruction.operands[0].mem.base))
            continue;
        assignments.push_back(i);
        if (instruction.operands[1].type != ZYDIS_OPERAND_TYPE_REGISTER)
            continue;
        const auto value = constant(image, code, i, instruction.operands[1].reg.value);
        if (value && table(image, *value))
            stores.emplace_back(i, *value);
    }
    if (!receiver) {
        std::set<std::uint64_t> unique;
        for (const auto& store : stores)
            unique.insert(store.second);
        return unique.size() == 1 ? std::optional(*unique.begin()) : std::nullopt;
    }
    if (returns.empty())
        return {};
    std::set<std::uint64_t> final;
    for (const auto& store : stores)
        if (std::all_of(returns.begin(), returns.end(),
                [&](auto end) { return image.dominates(code, store.first, end); }) &&
            std::all_of(assignments.begin(), assignments.end(),
                [&](auto prior) { return image.dominates(code, prior, store.first); }))
            final.insert(store.second);
    return final.size() == 1 ? std::optional(*final.begin()) : std::nullopt;
}

struct route {
    program request, getter, resolver;
    std::uint32_t config{}, routes{};
};

std::optional<std::uint32_t> route_field(const code_image& image, const program& code) {
    std::set<std::uint32_t> fields;
    for (std::size_t i = 0; i < code.instructions.size(); ++i) {
        const auto& instruction = code.instructions[i];
        if (instruction.instruction.mnemonic != ZYDIS_MNEMONIC_CMP || !member(instruction.operands[0], 8) ||
            instruction.operands[0].mem.disp.value != 0x19 ||
            instruction.operands[1].type != ZYDIS_OPERAND_TYPE_IMMEDIATE ||
            instruction.operands[1].imm.value.u)
            continue;
        const auto origins = image.origins(code, i, instruction.operands[0].mem.base);
        if (!origins || origins->size() != 1)
            continue;
        const auto parent = index(code, origins->front().instruction);
        if (!parent)
            continue;
        const auto& load = code.instructions[*parent];
        if (load.instruction.mnemonic != ZYDIS_MNEMONIC_MOV || !member(load.operands[1]) ||
            load.operands[1].mem.disp.value != 8)
            continue;
        const auto roots = image.origins(code, *parent, load.operands[1].mem.base);
        if (!roots || roots->size() != 1)
            continue;
        const auto root = index(code, roots->front().instruction);
        if (!root)
            continue;
        const auto& first = code.instructions[*root];
        if (first.instruction.mnemonic == ZYDIS_MNEMONIC_MOV && member(first.operands[1]) &&
            image.same_value(code, 0, ZYDIS_REGISTER_RDX, *root, first.operands[1].mem.base))
            fields.insert(static_cast<std::uint32_t>(first.operands[1].mem.disp.value));
    }
    return fields.size() == 1 ? std::optional(*fields.begin()) : std::nullopt;
}

std::vector<route> routes(const code_image& image, std::stop_token stop) {
    const auto env = image.references("{env}", stop), version = image.references("{version}", stop);
    const auto request = image.references("Failed to build the URL retrieving inventory elements", stop);
    if (env.status != code_status::complete || version.status != code_status::complete ||
        request.status != code_status::complete)
        return {};
    std::set<std::uint64_t> resolvers, consumers;
    for (const auto& e : env.values)
        for (const auto& v : version.values)
            if (e.function.begin == v.function.begin)
                resolvers.insert(image.base() + e.function.begin);
    std::vector<route> result;
    for (const auto& ref : request.values) {
        const auto outer = image.body(image.base() + ref.function.begin);
        if (!outer)
            continue;
        for (const auto& instruction : outer->instructions) {
            if (stop.stop_requested())
                return {};
            const auto target = call(instruction);
            const auto getter = target ? image.body(*target) : std::nullopt;
            if (!getter)
                continue;
            for (std::size_t i = 0; i < getter->instructions.size(); ++i) {
                const auto resolver_entry = call(getter->instructions[i]);
                if (!resolver_entry || !resolvers.contains(*resolver_entry) ||
                    consumers.contains(getter->instructions[i].address))
                    continue;
                const auto config = slice(*getter, i, ZYDIS_REGISTER_RDX);
                const auto resolver = image.body(*resolver_entry);
                if (!config || config->inputs != (1u << ZydisRegisterGetId(ZYDIS_REGISTER_RCX)) ||
                    config->code.instructions.empty() || !resolver)
                    continue;
                const auto& add = config->code.instructions.back();
                if (add.instruction.mnemonic != ZYDIS_MNEMONIC_ADD ||
                    add.operands[0].type != ZYDIS_OPERAND_TYPE_REGISTER ||
                    add.operands[0].reg.value != config->output ||
                    add.operands[1].type != ZYDIS_OPERAND_TYPE_IMMEDIATE || !add.operands[1].imm.value.u ||
                    add.operands[1].imm.value.u > 0x100000)
                    continue;
                const auto field = route_field(image, *resolver);
                if (!field)
                    continue;
                consumers.insert(getter->instructions[i].address);
                result.push_back({*outer, *getter, *resolver,
                    static_cast<std::uint32_t>(add.operands[1].imm.value.u), *field});
            }
        }
    }
    return result;
}
}

std::optional<inv_profile> discover_inventory(
    const locators::code_image& image, std::stop_token stop, const locators::validation_observer& observe) {
    const auto note = [&](std::string_view name, std::uint64_t address, std::uint64_t count) {
        if (observe)
            observe(name, address, count);
    };
    const auto paths = routes(image, stop);
    note("inventory_routes", 0, paths.size());
    if (paths.size() != 1 || stop.stop_requested())
        return {};
    const auto& path = paths.front();
    const auto control = type_table(image, path.resolver, false);
    note("inventory_control", path.resolver.begin, control.value_or(0));
    if (!control)
        return {};
    const auto references = image.references("FacadeInternal::m_criticalSection", stop);
    if (references.status != locators::code_status::complete)
        return {};
    std::vector<inv_profile> candidates;
    for (const auto& reference : references.values) {
        const auto factory = image.body(image.base() + reference.function.begin);
        if (!factory)
            continue;
        for (const auto& registry : locators::registries(*factory)) {
            const auto guard = index(*factory, registry.guard), append = index(*factory, registry.append);
            if (!guard || !append)
                continue;
            const auto origins =
                image.origins(*factory, *guard, factory->instructions[*append].operands[1].reg.value);
            if (!origins || origins->size() != 1)
                continue;
            const auto source = index(*factory, origins->front().instruction);
            if (!source)
                continue;
            const auto& add = factory->instructions[*source];
            if (add.instruction.mnemonic != ZYDIS_MNEMONIC_ADD || !member(add.operands[1]))
                continue;
            const auto delta = constant(image, *factory, *source, add.operands[0].reg.value);
            if (!delta || !*delta || *delta > 0x100000)
                continue;
            for (std::size_t i = 0; i < factory->instructions.size(); ++i) {
                if (stop.stop_requested())
                    return {};
                const auto& init = factory->instructions[i];
                if (init.instruction.mnemonic != ZYDIS_MNEMONIC_MOV || !member(init.operands[0]) ||
                    init.operands[0].mem.disp.value != static_cast<std::int64_t>(*delta) ||
                    init.operands[1].type != ZYDIS_OPERAND_TYPE_REGISTER)
                    continue;
                const auto entry_table = constant(image, *factory, i, init.operands[1].reg.value);
                if (!entry_table || !table(image, *entry_table))
                    continue;
                for (std::size_t j = 0; j < i; ++j) {
                    const auto target = call(factory->instructions[j]);
                    if (!target)
                        continue;
                    const auto arguments = image.origins(*factory, j, ZYDIS_REGISTER_RCX);
                    if (!arguments || arguments->size() != 1)
                        continue;
                    const auto argument = index(*factory, arguments->front().instruction);
                    if (!argument)
                        continue;
                    const auto& address = factory->instructions[*argument];
                    if (address.instruction.mnemonic != ZYDIS_MNEMONIC_LEA || !member(address.operands[1]) ||
                        address.operands[1].mem.disp.value != path.config ||
                        !image.same_value(
                            *factory, *argument, address.operands[1].mem.base, i, init.operands[0].mem.base))
                        continue;
                    const auto ctor = image.body(*target);
                    const auto config_table = ctor ? type_table(image, *ctor, true) : std::nullopt;
                    note("inventory_config_type", *target, config_table.value_or(0));
                    if (!config_table)
                        continue;
                    inv_profile profile{registry, *entry_table, *config_table, *control,
                        static_cast<std::uint32_t>(*delta), path.config, path.routes, {}};
                    std::set<std::uint64_t> functions;
                    for (const auto* code :
                        {&*factory, &path.request, &path.getter, &path.resolver, &*ctor}) {
                        functions.insert(code->begin);
                        for (const auto& instruction : code->instructions)
                            if (const auto callee = call(instruction); callee && image.no_return(*callee))
                                functions.insert(*callee);
                    }
                    for (auto entry : functions) {
                        const auto function = image.function(entry);
                        if (!function || image.base() + function->begin != entry)
                            return {};
                        const auto bytes = image.bytes(entry, function->end - function->begin);
                        if (bytes.size() != function->end - function->begin)
                            return {};
                        profile.proofs.push_back(
                            {function->begin, function->end, diagnostics::sha256(bytes)});
                    }
                    candidates.push_back(std::move(profile));
                }
            }
        }
    }
    note("inventory_profiles", 0, candidates.size());
    return candidates.size() == 1 ? std::optional(std::move(candidates.front())) : std::nullopt;
}
}
