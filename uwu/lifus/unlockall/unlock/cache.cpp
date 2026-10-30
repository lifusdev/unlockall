#include "lifus/unlockall/unlock/Catalog.h"
#include "lifus/unlockall/unlock/Cfg.h"
#include "lifus/unlockall/log/Log.h"
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <wincrypt.h>
#include <roapi.h>
#include <miniz.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <set>
#include <stdexcept>

namespace uwu::sdk {
namespace {
constexpr std::size_t decoded_limit = 128 * 1024 * 1024;

struct apartment {
    HRESULT result = RoInitialize(RO_INIT_MULTITHREADED);

    apartment() {
        if (FAILED(result) && result != RPC_E_CHANGED_MODE)
            throw std::runtime_error("catalog json initialization failed");
    }

    ~apartment() {
        winrt::clear_factory_cache();
        if (SUCCEEDED(result))
            RoUninitialize();
    }
};

std::string file(const std::filesystem::path& path, std::size_t limit) {
    const auto size = std::filesystem::file_size(path);
    if (!size || size > limit)
        throw std::runtime_error("item cache file size is invalid");
    std::string result(static_cast<std::size_t>(size), '\0');
    std::ifstream input(path, std::ios::binary);
    if (!input.read(result.data(), static_cast<std::streamsize>(result.size())))
        throw std::runtime_error("item cache read failed");
    return result;
}

std::string normalize_hash(std::string_view value) {
    while (!value.empty() &&
        (value.back() == '\r' || value.back() == '\n' || value.back() == ' ' || value.back() == '\t'))
        value.remove_suffix(1);
    if (value.size() != 64)
        throw std::runtime_error("item cache hash is invalid");
    std::string result(value);
    for (auto& c : result) {
        if (c >= 'A' && c <= 'F')
            c += 'a' - 'A';
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            throw std::runtime_error("item cache hash is invalid");
    }
    return result;
}

std::string identifier(std::wstring_view value) {
    if (value.size() != 36)
        throw std::runtime_error("item cache identifier is invalid");
    std::string result;
    for (std::size_t i = 0; i < value.size(); ++i) {
        auto c = value[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != L'-')
                throw std::runtime_error("item cache identifier is invalid");
        } else {
            if (c >= L'A' && c <= L'F')
                c += L'a' - L'A';
            if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f')))
                throw std::runtime_error("item cache identifier is invalid");
        }
        result.push_back(static_cast<char>(c));
    }
    return result;
}

std::vector<std::byte> decode(std::string_view encoded) {
    DWORD size{}, skipped{}, flags{};
    const DWORD format = CRYPT_STRING_BASE64 | CRYPT_STRING_STRICT;
    if (encoded.size() > inv::maximum_bytes ||
        !CryptStringToBinaryA(
            encoded.data(), static_cast<DWORD>(encoded.size()), format, nullptr, &size, &skipped, &flags) ||
        skipped || !size || size > inv::maximum_bytes)
        throw std::runtime_error("item cache base64 is invalid");
    std::vector<std::byte> result(size);
    if (!CryptStringToBinaryA(encoded.data(), static_cast<DWORD>(encoded.size()), format,
            reinterpret_cast<BYTE*>(result.data()), &size, &skipped, &flags) ||
        skipped || size != result.size())
        throw std::runtime_error("item cache base64 decode failed");
    return result;
}

std::string inflate(std::span<const std::byte> bytes) {
    const auto u8 = [&](std::size_t at) { return std::to_integer<unsigned>(bytes[at]); };
    const auto u32 = [&](std::size_t at) {
        std::uint32_t value{};
        std::memcpy(&value, bytes.data() + at, sizeof(value));
        return value;
    };
    if (bytes.size() < 18 || u8(0) != 31 || u8(1) != 139 || u8(2) != 8 || (u8(3) & 0xe0))
        throw std::runtime_error("item cache gzip header is invalid");
    const auto end = bytes.size() - 8;
    std::size_t offset = 10;
    if (u8(3) & 4) {
        if (offset + 2 > end)
            throw std::runtime_error("item cache gzip extra header is invalid");
        const auto size = u8(offset) | (u8(offset + 1) << 8);
        offset += 2;
        if (size > end - offset)
            throw std::runtime_error("item cache gzip extra header is invalid");
        offset += size;
    }
    for (const unsigned flag : {8u, 16u})
        if (u8(3) & flag) {
            while (offset < end && bytes[offset] != std::byte{})
                ++offset;
            if (offset == end)
                throw std::runtime_error("item cache gzip text header is invalid");
            ++offset;
        }
    if (u8(3) & 2) {
        if (offset + 2 > end ||
            (mz_crc32(0, reinterpret_cast<const unsigned char*>(bytes.data()), offset) & 0xffffu) !=
                (u8(offset) | (u8(offset + 1) << 8)))
            throw std::runtime_error("item cache gzip header checksum is invalid");
        offset += 2;
    }
    const auto size = u32(bytes.size() - 4);
    if (offset >= end || !size || size > decoded_limit)
        throw std::runtime_error("item cache decompression bounds");
    std::string result(size, '\0');
    tinfl_decompressor state{};
    tinfl_init(&state);
    auto input_size = end - offset;
    auto output_size = result.size();
    const auto status = tinfl_decompress(&state, reinterpret_cast<const mz_uint8*>(bytes.data() + offset),
        &input_size, reinterpret_cast<mz_uint8*>(result.data()), reinterpret_cast<mz_uint8*>(result.data()),
        &output_size, TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    if (status != TINFL_STATUS_DONE || input_size != end - offset || output_size != size ||
        mz_crc32(0, reinterpret_cast<const unsigned char*>(result.data()), result.size()) != u32(end))
        throw std::runtime_error("item cache gzip validation failed");
    return result;
}
}

std::filesystem::path find_item_cache(const std::filesystem::path& executable) {
    if (!executable.is_absolute())
        throw std::runtime_error("game path is not absolute");
    auto directory = executable.parent_path();
    std::filesystem::path selected;
    for (unsigned depth = 0; depth < 4 && !directory.empty(); ++depth) {
        const auto candidate = directory / "download" / "shop" / "items";
        if (std::filesystem::is_regular_file(candidate / "content") &&
            std::filesystem::is_regular_file(candidate / "hash")) {
            const auto path = std::filesystem::canonical(candidate);
            if (!selected.empty() && selected != path)
                throw std::runtime_error("multiple game item caches");
            selected = path;
        }
        const auto parent = directory.parent_path();
        if (parent == directory)
            break;
        directory = parent;
    }
    if (selected.empty())
        throw std::runtime_error("game item cache is unavailable");
    return selected;
}

std::string read_item_cache(const std::filesystem::path& directory) {
    const auto content_path = directory / "content", hash_path = directory / "hash";
    const auto content_time = std::filesystem::last_write_time(content_path);
    const auto hash_time = std::filesystem::last_write_time(hash_path);
    const auto encoded = file(content_path, inv::maximum_bytes);
    const auto hash_text = file(hash_path, 128);
    const auto expected = normalize_hash(hash_text);
    const auto compressed = decode(encoded);
    if (diagnostics::sha256(compressed) != expected)
        throw std::runtime_error("item cache hash mismatch");
    const auto json = inflate(compressed);
    apartment initialized;
    std::string result =
        "{\"verified\":true,\"source\":\"game item cache\",\"sha256\":\"" + expected + "\",\"items\":[";
    try {
        const auto document = winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(json));
        const auto items = document.GetNamedArray(L"items");
        if (!items.Size() || items.Size() > inv::maximum_items)
            throw std::runtime_error("item cache count is invalid");
        std::set<std::string> unique;
        for (const auto& value : items) {
            const auto item = value.GetObject();
            const auto id = identifier(item.GetNamedString(L"itemId"));
            const auto space = identifier(item.GetNamedString(L"spaceId"));
            const auto type = item.GetNamedString(L"type");
            if (type.empty() || type.size() > 256 || !unique.insert(id).second)
                throw std::runtime_error("item cache entries are invalid");
            if (unique.size() > 1)
                result += ',';
            result += "{\"itemId\":\"" + id + "\",\"spaceId\":\"" + space + "\"}";
        }
    } catch (const winrt::hresult_error&) {
        throw std::runtime_error("item cache json or schema is invalid");
    }
    if (std::filesystem::last_write_time(content_path) != content_time ||
        std::filesystem::last_write_time(hash_path) != hash_time ||
        file(content_path, inv::maximum_bytes) != encoded || file(hash_path, 128) != hash_text)
        throw std::runtime_error("item cache changed during export");
    return result + "]}";
}
}
