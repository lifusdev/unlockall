#include "lifus/unlockall/unlock/Inventory.h"
#include "lifus/unlockall/unlock/Cfg.h"
#include "lifus/unlockall/log/Log.h"
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <roapi.h>
#include <cmath>
#include <fstream>
#include <set>
#include <stdexcept>
#include <array>
#include <cstdio>
#include <optional>

namespace uwu {
namespace {
struct apartment {
    HRESULT result = RoInitialize(RO_INIT_MULTITHREADED);

    ~apartment() {
        if (SUCCEEDED(result)) {
            winrt::clear_factory_cache();
            RoUninitialize();
        }
    }
};

void initialize_apartment() {
    thread_local apartment state_apartment;
    if (FAILED(state_apartment.result) && state_apartment.result != RPC_E_CHANGED_MODE)
        winrt::check_hresult(state_apartment.result);
}

std::string read_document(const std::filesystem::path& path, bool allow_empty = false) {
    const auto size = std::filesystem::file_size(path);
    if ((!size && !allow_empty) || size > inv::maximum_bytes)
        throw std::runtime_error("inventory document size is invalid");
    std::string body(static_cast<std::size_t>(size), '\0');
    std::ifstream file(path, std::ios::binary);
    if (!file.read(body.data(), static_cast<std::streamsize>(size)))
        throw std::runtime_error("inventory document read failed");
    return body;
}

bool guid(std::wstring_view text) {
    if (text.size() != 36)
        return false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto c = text[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != L'-')
                return false;
        } else if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F')))
            return false;
    }
    return true;
}
}

inventory load_inventory(const std::filesystem::path& path) {
    inventory result;
    result.modified = std::filesystem::last_write_time(path);
    const auto bytes = std::filesystem::file_size(path);
    if (!bytes || bytes > inv::maximum_bytes)
        throw std::runtime_error("inventory file size is invalid");
    std::ifstream file(path, std::ios::binary);
    result.body.resize(static_cast<std::size_t>(bytes));
    if (!file.read(result.body.data(), static_cast<std::streamsize>(bytes)))
        throw std::runtime_error("inventory file read failed");
    try {
        initialize_apartment();
        using namespace winrt::Windows::Data::Json;
        const auto document = JsonObject::Parse(winrt::to_hstring(result.body));
        const auto items = document.GetNamedArray(L"items");
        if (!items.Size() || items.Size() > inv::maximum_items)
            throw std::runtime_error("inventory item count is invalid");
        std::set<std::wstring> identifiers;
        for (const auto& entry : items) {
            const auto item = entry.GetObject();
            const auto id = item.GetNamedString(L"itemId");
            if (!guid(id))
                throw std::runtime_error("inventory itemid is invalid");
            std::wstring normalized(id);
            for (auto& ch : normalized)
                if (ch >= L'A' && ch <= L'F')
                    ch += L'a' - L'A';
            if (!identifiers.insert(std::move(normalized)).second)
                throw std::runtime_error("inventory contains duplicate itemids");
            const auto quantity = item.GetNamedNumber(L"quantity");
            if (!std::isfinite(quantity) || quantity < 0 || quantity > 2147483647 ||
                std::floor(quantity) != quantity)
                throw std::runtime_error("inventory quantity is invalid");
            const auto modified = item.GetNamedString(L"lastModified");
            if (modified.empty() || modified.size() > 64)
                throw std::runtime_error("inventory lastmodified is invalid");
            const auto expiration = item.GetNamedValue(L"expirationDate");
            if (expiration.ValueType() != JsonValueType::Null &&
                (expiration.ValueType() != JsonValueType::String || expiration.GetString().size() > 64))
                throw std::runtime_error("inventory expirationdate is invalid");
            const auto details = item.GetNamedValue(L"expirationDetails");
            if (details.ValueType() != JsonValueType::Null && details.ValueType() != JsonValueType::Object)
                throw std::runtime_error("inventory expirationdetails is invalid");
        }
        result.items = items.Size();
    } catch (const winrt::hresult_error&) {
        throw std::runtime_error("inventory json or schema is invalid");
    }
    if (std::filesystem::last_write_time(path) != result.modified ||
        std::filesystem::file_size(path) != bytes)
        throw std::runtime_error("inventory changed during validation");
    const auto digest = diagnostics::sha256(std::as_bytes(std::span(result.body.data(), result.body.size())));
    if (digest.empty())
        throw std::runtime_error("inventory digest failed");
    result.health =
        "{\"local\":true,\"items\":" + std::to_string(result.items) + ",\"sha256\":\"" + digest + "\"}";
    return result;
}

std::size_t generate_inventory(const std::filesystem::path& inventory, const std::filesystem::path& catalog) {
    initialize_apartment();
    const auto original = std::filesystem::exists(inventory)
        ? std::optional<std::string>(read_document(inventory, true))
        : std::nullopt;
    const auto catalog_body = read_document(catalog);
    using namespace winrt::Windows::Data::Json;
    const auto catalog_document = JsonObject::Parse(winrt::to_hstring(catalog_body));
    if (!catalog_document.GetNamedBoolean(L"verified"))
        throw std::runtime_error("catalog is not verified");
    const auto catalog_items = catalog_document.GetNamedArray(L"items");
    if (!catalog_items.Size() || catalog_items.Size() > inv::maximum_items)
        throw std::runtime_error("catalog count is invalid");
    const auto normalize = [](std::wstring_view value) {
        if (!guid(value))
            throw std::runtime_error("catalog identifier is invalid");
        std::wstring result(value);
        for (auto& character : result)
            if (character >= L'A' && character <= L'F')
                character += L'a' - L'A';
        return result;
    };
    std::set<std::wstring> catalog_ids;
    for (const auto& value : catalog_items) {
        const auto item = value.GetObject();
        const auto identifier = normalize(item.GetNamedString(L"itemId"));
        static_cast<void>(normalize(item.GetNamedString(L"spaceId")));
        if (!catalog_ids.insert(identifier).second)
            throw std::runtime_error("catalog contains duplicate itemids");
    }
    JsonObject document;
    JsonArray items;
    document.Insert(L"items", items);
    SYSTEMTIME time{};
    GetSystemTime(&time);
    std::array<char, 32> timestamp{};
    sprintf_s(timestamp.data(), timestamp.size(), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", time.wYear,
        time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
    for (const auto& identifier : catalog_ids) {
        JsonObject item;
        item.Insert(L"itemId", JsonValue::CreateStringValue(identifier));
        item.Insert(L"quantity", JsonValue::CreateNumberValue(1));
        item.Insert(L"lastModified", JsonValue::CreateStringValue(winrt::to_hstring(timestamp.data())));
        item.Insert(L"expirationDate", JsonValue::CreateNullValue());
        item.Insert(L"expirationDetails", JsonValue::CreateNullValue());
        items.Append(item);
    }
    const auto body = winrt::to_string(document.Stringify());
    if (body.size() > inv::maximum_bytes)
        throw std::runtime_error("generated inventory exceeds size limit");
    auto temporary = inventory;
    temporary += L".update.tmp";

    struct temporary_file {
        std::filesystem::path path;

        ~temporary_file() {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    } cleanup{temporary};

    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output.write(body.data(), static_cast<std::streamsize>(body.size())) || !output.flush())
        throw std::runtime_error("inventory update write failed");
    output.close();
    static_cast<void>(load_inventory(temporary));
    if (original) {
        if (read_document(inventory, true) != *original)
            throw std::runtime_error("inventory changed during generation");
        auto backup = inventory;
        backup += L".previous";
        if (!ReplaceFileW(inventory.c_str(), temporary.c_str(), backup.c_str(),
                REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr))
            throw std::runtime_error("inventory replacement failed");
    } else if (!MoveFileExW(temporary.c_str(), inventory.c_str(), MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("inventory creation failed");
    return items.Size();
}
}
