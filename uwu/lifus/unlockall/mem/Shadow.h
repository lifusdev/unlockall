#pragma once
#include "lifus/unlockall/mem/Memory.h"
#include <map>
#include <set>

namespace uwu {
class shadow_memory {
  public:
    struct limits {
        std::size_t bytes = 16384, reads = 512, writes = 64;
    };

    struct write_record {
        std::uint64_t instruction{}, address{};
        std::size_t size{};
    };

    struct read_record {
        std::uint64_t address{};
        std::vector<std::byte> bytes;
    };

    explicit shadow_memory(const memory& process);
    shadow_memory(const memory& process, limits budget);
    void pin(std::uint64_t address, std::span<const std::byte> bytes);
    void allow_write(std::uint64_t instruction);
    void protect(std::uint64_t address, std::size_t size);
    [[nodiscard]] read_result read_bytes(std::uint64_t address, std::span<std::byte> output);
    [[nodiscard]] DWORD write_bytes(
        std::uint64_t instruction, std::uint64_t address, std::span<const std::byte> input);
    [[nodiscard]] DWORD verify();
    [[nodiscard]] std::vector<read_record> inputs() const;

    [[nodiscard]] std::span<const write_record> writes() const noexcept { return writes_; }

    [[nodiscard]] std::size_t captured_bytes() const noexcept { return cells_.size() + pinned_bytes_; }

  private:
    struct cell {
        std::byte original{}, value{};
    };

    const memory& process_;
    limits budget_;
    std::map<std::uint64_t, cell> cells_;
    std::set<std::uint64_t> allowed_;
    std::vector<std::pair<std::uint64_t, std::size_t>> protected_;
    std::vector<write_record> writes_;
    std::vector<read_record> pinned_;
    std::size_t pinned_bytes_{};
    std::size_t reads_{};
    DWORD failure_{};
    DWORD capture(std::uint64_t address, std::size_t size);
};
}
