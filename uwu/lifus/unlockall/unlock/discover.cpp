#include "lifus/unlockall/unlock/Profile.h"
#include "lifus/unlockall/log/Log.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <system_error>

namespace uwu::sdk {
inv_profile read_inventory_profile(const memory& process, const module_info& module) {
    const auto read = [&](std::uint32_t rva, std::size_t size) {
        if (!size || rva >= module.size || size > module.size - rva || size > 512ull * 1024 * 1024)
            throw std::runtime_error("inventory image bounds");
        std::vector<std::byte> bytes(size);
        for (std::size_t offset = 0; offset < size;) {
            const auto count = std::min<std::size_t>(0x400000, size - offset);
            const auto result =
                process.read_bytes(module.address + rva + offset, std::span(bytes).subspan(offset, count));
            if (!result.complete())
                throw std::system_error(result.error, std::system_category(), "inventory image read");
            offset += count;
        }
        return bytes;
    };
    const auto dos = process.read<IMAGE_DOS_HEADER>(module.address);
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
        static_cast<std::size_t>(dos->e_lfanew) > module.size ||
        sizeof(IMAGE_NT_HEADERS64) > module.size - static_cast<std::size_t>(dos->e_lfanew))
        throw std::runtime_error("inventory dos header");
    const auto nt = process.read<IMAGE_NT_HEADERS64>(module.address + dos->e_lfanew);
    if (!nt || nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt->OptionalHeader.SizeOfImage > module.size ||
        nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXCEPTION ||
        !nt->FileHeader.NumberOfSections || nt->FileHeader.NumberOfSections > 96)
        throw std::runtime_error("inventory nt header");
    const auto range = [&](std::uint32_t rva, std::size_t size) {
        return size && rva < nt->OptionalHeader.SizeOfImage && size <= nt->OptionalHeader.SizeOfImage - rva;
    };
    const auto table = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (!range(table.VirtualAddress, table.Size) || table.Size > 32 * 1024 * 1024 ||
        table.Size % sizeof(locators::function_record) ||
        !range(nt->OptionalHeader.BaseOfCode, nt->OptionalHeader.SizeOfCode) ||
        nt->OptionalHeader.SizeOfCode > 512 * 1024 * 1024)
        throw std::runtime_error("inventory code bounds");
    const auto section_rva =
        static_cast<std::uint64_t>(dos->e_lfanew) + 24 + nt->FileHeader.SizeOfOptionalHeader;
    if (section_rva > UINT32_MAX)
        throw std::runtime_error("inventory section bounds");
    const auto raw_sections = read(static_cast<std::uint32_t>(section_rva),
        nt->FileHeader.NumberOfSections * sizeof(IMAGE_SECTION_HEADER));
    std::vector<IMAGE_SECTION_HEADER> sections(nt->FileHeader.NumberOfSections);
    std::memcpy(sections.data(), raw_sections.data(), raw_sections.size());
    std::optional<IMAGE_SECTION_HEADER> data;
    for (const auto& section : sections)
        if (!std::memcmp(section.Name, ".rdata", 6)) {
            if (data || !range(section.VirtualAddress, section.Misc.VirtualSize) ||
                section.Misc.VirtualSize > 32 * 1024 * 1024)
                throw std::runtime_error("inventory readonly section bounds");
            data = section;
        }
    if (!data)
        throw std::runtime_error("inventory readonly section missing");
    const auto code = read(nt->OptionalHeader.BaseOfCode, nt->OptionalHeader.SizeOfCode);
    const auto names = read(data->VirtualAddress, data->Misc.VirtualSize);
    const auto raw_functions = read(table.VirtualAddress, table.Size);
    std::vector<locators::function_record> functions(
        raw_functions.size() / sizeof(locators::function_record));
    std::memcpy(functions.data(), raw_functions.data(), raw_functions.size());
    for (const auto& function : functions)
        if (function.end > nt->OptionalHeader.SizeOfImage)
            throw std::runtime_error("inventory function bounds");
    const locators::code_image image(
        code, nt->OptionalHeader.BaseOfCode, module.address, functions, names, data->VirtualAddress);
    auto profile = discover_inventory(image, {}, [&](auto stage, auto address, auto value) {
        diagnostics::write(
            stage, {{"address", diagnostics::hex(address)}, {"value", diagnostics::hex(value)}});
    });
    if (!profile)
        throw std::runtime_error("inventory native relationships are missing or ambiguous");
    return std::move(*profile);
}
}
