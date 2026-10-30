#include "lifus/unlockall/mem/Shadow.h"
#include <algorithm>
#include <stdexcept>

namespace uwu {
namespace {
bool valid(std::uint64_t address, std::size_t size) {
    return address >= 0x10000 && address < 0x800000000000ull && size && size <= 0x800000000000ull - address;
}

bool overlaps(std::uint64_t a, std::size_t size_a, std::uint64_t b, std::size_t size_b) {
    return a < b + size_b && b < a + size_a;
}
}

shadow_memory::shadow_memory(const memory& process) : shadow_memory(process, limits{}) {}

shadow_memory::shadow_memory(const memory& process, limits budget) : process_(process), budget_(budget) {
    if (!budget.bytes || budget.bytes > 1048576 || !budget.reads || budget.reads > 65536 || !budget.writes ||
        budget.writes > 4096)
        throw std::invalid_argument("invalid shadow budget");
}

void shadow_memory::allow_write(std::uint64_t instruction) {
    if (!valid(instruction, 1) || allowed_.size() >= 256)
        throw std::invalid_argument("invalid shadow instruction");
    allowed_.insert(instruction);
}

void shadow_memory::pin(std::uint64_t address, std::span<const std::byte> bytes) {
    if (failure_ || !valid(address, bytes.size()) || bytes.size() > budget_.bytes - captured_bytes() ||
        pinned_.size() >= 256)
        throw std::invalid_argument("invalid shadow snapshot");
    const auto after = cells_.lower_bound(address);
    if ((after != cells_.end() && after->first < address + bytes.size()) ||
        std::any_of(pinned_.begin(), pinned_.end(), [&](const auto& region) {
            return overlaps(address, bytes.size(), region.address, region.bytes.size());
        }))
        throw std::invalid_argument("overlapping shadow snapshot");
    protect(address, bytes.size());
    pinned_.push_back({address, {bytes.begin(), bytes.end()}});
    pinned_bytes_ += bytes.size();
}

void shadow_memory::protect(std::uint64_t address, std::size_t size) {
    if (!valid(address, size) || protected_.size() >= 256)
        throw std::invalid_argument("invalid shadow protection");
    if (std::any_of(writes_.begin(), writes_.end(),
            [&](const auto& write) { return overlaps(address, size, write.address, write.size); })) {
        failure_ = ERROR_ACCESS_DENIED;
        throw std::runtime_error("shadow write overlaps protected data");
    }
    protected_.emplace_back(address, size);
}

DWORD shadow_memory::capture(std::uint64_t address, std::size_t size) {
    if (failure_)
        return failure_;
    if (!valid(address, size))
        return failure_ = ERROR_INVALID_ADDRESS;
    if (size > budget_.bytes)
        return failure_ = ERROR_NOT_ENOUGH_QUOTA;
    std::vector<std::pair<std::uint64_t, std::byte>> pending;
    for (std::size_t offset = 0; offset < size;) {
        if (cells_.contains(address + offset)) {
            ++offset;
            continue;
        }
        const auto begin = offset;
        while (offset < size && !cells_.contains(address + offset))
            ++offset;
        const auto count = offset - begin;
        if (captured_bytes() + pending.size() + count > budget_.bytes || reads_ >= budget_.reads)
            return failure_ = ERROR_NOT_ENOUGH_QUOTA;
        std::vector<std::byte> bytes(count);
        ++reads_;
        const auto read = process_.read_bytes(address + begin, bytes);
        if (!read.complete() || read.transferred != count)
            return failure_ = read.error ? read.error : ERROR_PARTIAL_COPY;
        for (std::size_t i = 0; i < count; ++i)
            pending.emplace_back(address + begin + i, bytes[i]);
    }
    for (const auto& [at, value] : pending)
        cells_.emplace(at, cell{value, value});
    return ERROR_SUCCESS;
}

read_result shadow_memory::read_bytes(std::uint64_t address, std::span<std::byte> output) {
    if (failure_)
        return {0, failure_};
    if (!valid(address, output.size()))
        return {0, failure_ = ERROR_INVALID_ADDRESS};
    for (const auto& region : pinned_)
        if (overlaps(address, output.size(), region.address, region.bytes.size())) {
            if (address < region.address || address - region.address > region.bytes.size() ||
                output.size() > region.bytes.size() - static_cast<std::size_t>(address - region.address))
                return {0, failure_ = ERROR_INVALID_ADDRESS};
            std::copy_n(region.bytes.begin() + static_cast<std::size_t>(address - region.address),
                output.size(), output.begin());
            return {output.size(), ERROR_SUCCESS};
        }
    if (const auto error = capture(address, output.size()))
        return {0, error};
    for (std::size_t i = 0; i < output.size(); ++i)
        output[i] = cells_.at(address + i).value;
    return {output.size(), ERROR_SUCCESS};
}

DWORD shadow_memory::write_bytes(
    std::uint64_t instruction, std::uint64_t address, std::span<const std::byte> input) {
    if (failure_)
        return failure_;
    if (!valid(address, input.size()))
        return failure_ = ERROR_INVALID_ADDRESS;
    if (!allowed_.contains(instruction) ||
        std::any_of(protected_.begin(), protected_.end(),
            [&](const auto& region) { return overlaps(address, input.size(), region.first, region.second); }))
        return failure_ = ERROR_ACCESS_DENIED;
    if (writes_.size() >= budget_.writes)
        return failure_ = ERROR_NOT_ENOUGH_QUOTA;
    if (const auto error = capture(address, input.size()))
        return error;
    for (std::size_t i = 0; i < input.size(); ++i)
        cells_.at(address + i).value = input[i];
    writes_.push_back({instruction, address, input.size()});
    return ERROR_SUCCESS;
}

DWORD shadow_memory::verify() {
    if (failure_)
        return failure_;
    for (const auto& region : pinned_) {
        if (reads_ >= budget_.reads)
            return failure_ = ERROR_NOT_ENOUGH_QUOTA;
        std::vector<std::byte> observed(region.bytes.size());
        ++reads_;
        const auto read = process_.read_bytes(region.address, observed);
        if (!read.complete() || read.transferred != observed.size())
            return failure_ = read.error ? read.error : ERROR_PARTIAL_COPY;
        if (observed != region.bytes)
            return failure_ = ERROR_RETRY;
    }
    for (auto it = cells_.begin(); it != cells_.end();) {
        const auto start = it->first;
        std::vector<std::byte> expected;
        do {
            expected.push_back(it->second.original);
            ++it;
        } while (it != cells_.end() && it->first == start + expected.size());
        if (reads_ >= budget_.reads)
            return failure_ = ERROR_NOT_ENOUGH_QUOTA;
        std::vector<std::byte> observed(expected.size());
        ++reads_;
        const auto read = process_.read_bytes(start, observed);
        if (!read.complete() || read.transferred != observed.size())
            return failure_ = read.error ? read.error : ERROR_PARTIAL_COPY;
        if (observed != expected)
            return failure_ = ERROR_RETRY;
    }
    return ERROR_SUCCESS;
}

std::vector<shadow_memory::read_record> shadow_memory::inputs() const {
    std::vector<read_record> result;
    for (const auto& [address, cell] : cells_) {
        if (result.empty() || address != result.back().address + result.back().bytes.size())
            result.push_back({address, {}});
        result.back().bytes.push_back(cell.original);
    }
    result.insert(result.end(), pinned_.begin(), pinned_.end());
    std::sort(
        result.begin(), result.end(), [](const auto& a, const auto& b) { return a.address < b.address; });
    std::vector<read_record> merged;
    for (auto& region : result) {
        if (!merged.empty() && merged.back().address + merged.back().bytes.size() == region.address)
            merged.back().bytes.insert(merged.back().bytes.end(), region.bytes.begin(), region.bytes.end());
        else
            merged.push_back(std::move(region));
    }
    return merged;
}
}
