#include "lifus/unlockall/log/Zip.h"
#include "lifus/unlockall/mem/Handle.h"
#include <miniz.h>
#include <array>
#include <cstring>
#include <limits>

namespace uwu::diagnostics {
namespace {
constexpr std::string_view member = "diagnostic.jsonl";
constexpr std::uint64_t header_size = 30 + member.size();

struct header {
    std::array<std::byte, 256> bytes{};
    std::size_t size{};

    void number(std::uint64_t value_data, unsigned width = 2) noexcept {
        for (unsigned i = 0; i < width; ++i)
            bytes[size++] = std::byte((value_data >> (i * 8)) & 255);
    }

    void name() noexcept {
        std::memcpy(bytes.data() + size, member.data(), member.size());
        size += member.size();
    }
};
}

struct archive::impl {
    unique_handle file;
    std::unique_ptr<tdefl_compressor> compressor = std::make_unique<tdefl_compressor>();
    std::uint64_t position{}, uncompressed{};
    mz_ulong crc{};
    DWORD error{};
    std::uint16_t time{}, date{};

    bool seek(std::uint64_t at) noexcept {
        LARGE_INTEGER offset{};
        offset.QuadPart = static_cast<LONGLONG>(at);
        if (SetFilePointerEx(file.get(), offset, nullptr, FILE_BEGIN))
            return true;
        error = GetLastError();
        return false;
    }

    bool write(const void* bytes_value, std::size_t size_value) noexcept {
        DWORD written{};
        const auto result_value =
            WriteFile(file.get(), bytes_value, static_cast<DWORD>(size_value), &written, nullptr);
        if (!result_value || written != size_value) {
            error = result_value ? ERROR_WRITE_FAULT : GetLastError();
            return false;
        }
        position += written;
        return true;
    }

    DWORD checkpoint() noexcept {
        if (!file)
            return error ? error : ERROR_INVALID_HANDLE;
        const auto end_value = position;
        if (end_value < header_size || end_value > UINT32_MAX - 256 || uncompressed > UINT32_MAX)
            return error = ERROR_FILE_TOO_LARGE;
        const auto compressed = end_value - header_size + 5;
        header tail;
        tail.number(1, 1);
        tail.number(0);
        tail.number(0xffff);
        tail.number(0x08074b50, 4);
        tail.number(crc, 4);
        tail.number(compressed, 4);
        tail.number(uncompressed, 4);
        const auto directory_value = end_value + tail.size;
        const auto begin_value = tail.size;
        tail.number(0x02014b50, 4);
        tail.number(20);
        tail.number(20);
        tail.number(8);
        tail.number(8);
        tail.number(time);
        tail.number(date);
        tail.number(crc, 4);
        tail.number(compressed, 4);
        tail.number(uncompressed, 4);
        tail.number(member.size());
        tail.number(0);
        tail.number(0);
        tail.number(0);
        tail.number(0);
        tail.number(0, 4);
        tail.number(0, 4);
        tail.name();
        const auto directory_size = tail.size - begin_value;
        tail.number(0x06054b50, 4);
        tail.number(0);
        tail.number(0);
        tail.number(1);
        tail.number(1);
        tail.number(directory_size, 4);
        tail.number(directory_value, 4);
        tail.number(0);
        if (!write(tail.bytes.data(), tail.size))
            return error;
        if (!SetEndOfFile(file.get()))
            return error = GetLastError();
        if (!seek(end_value))
            return error;
        position = end_value;
        return ERROR_SUCCESS;
    }
};

archive::archive() = default;

archive::~archive() {
    close();
}

DWORD archive::open(const std::wstring& path_value) {
    close();
    const auto attributes = GetFileAttributesW(path_value.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES &&
        attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
        return ERROR_ACCESS_DENIED;
    impl_ = std::make_unique<impl>();
    auto& state_value = *impl_;
    state_value.file.reset(CreateFileW(path_value.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!state_value.file)
        return state_value.error = GetLastError();
    SYSTEMTIME now{};
    GetSystemTime(&now);
    state_value.time = static_cast<std::uint16_t>((now.wHour << 11) | (now.wMinute << 5) | (now.wSecond / 2));
    state_value.date = static_cast<std::uint16_t>(((now.wYear - 1980) << 9) | (now.wMonth << 5) | now.wDay);
    header header_value;
    header_value.number(0x04034b50, 4);
    header_value.number(20);
    header_value.number(8);
    header_value.number(8);
    header_value.number(state_value.time);
    header_value.number(state_value.date);
    header_value.number(0, 4);
    header_value.number(0, 4);
    header_value.number(0, 4);
    header_value.number(member.size());
    header_value.number(0);
    header_value.name();
    if (!state_value.write(header_value.bytes.data(), header_value.size)) {
        state_value.file.reset();
        return state_value.error;
    }
    const auto status_value = tdefl_init(
        state_value.compressor.get(),
        [](const void* bytes_value, int size_value, void* context) -> mz_bool {
            return static_cast<impl*>(context)->write(bytes_value, static_cast<std::size_t>(size_value));
        },
        &state_value, TDEFL_GREEDY_PARSING_FLAG | 1);
    if (status_value != TDEFL_STATUS_OKAY) {
        state_value.file.reset();
        return state_value.error = ERROR_INVALID_DATA;
    }
    return append({});
}

DWORD archive::append(std::string_view text_value) noexcept {
    if (!*this)
        return ERROR_INVALID_HANDLE;
    auto& state_value = *impl_;
    const auto status_value = tdefl_compress_buffer(
        state_value.compressor.get(), text_value.data(), text_value.size(), TDEFL_SYNC_FLUSH);
    if (status_value != TDEFL_STATUS_OKAY)
        return state_value.error ? state_value.error : ERROR_INVALID_DATA;
    state_value.crc = mz_crc32(
        state_value.crc, reinterpret_cast<const unsigned char*>(text_value.data()), text_value.size());
    state_value.uncompressed += text_value.size();
    return state_value.checkpoint();
}

DWORD archive::flush() noexcept {
    if (!*this)
        return ERROR_SUCCESS;
    if (FlushFileBuffers(impl_->file.get()))
        return ERROR_SUCCESS;
    return impl_->error = GetLastError();
}

DWORD archive::close() noexcept {
    const auto error_value = flush();
    if (impl_)
        impl_->file.reset();
    return error_value;
}

archive::operator bool() const noexcept {
    return impl_ && static_cast<bool>(impl_->file);
}
}
