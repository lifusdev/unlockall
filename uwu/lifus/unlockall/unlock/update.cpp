#include "lifus/unlockall/unlock/Update.h"
#include "lifus/unlockall/unlock/Catalog.h"
#include "lifus/unlockall/unlock/Inventory.h"
#include "lifus/unlockall/log/Log.h"
#include "lifus/unlockall/mem/Handle.h"
#include <stdexcept>

namespace uwu {
int update_inventory_from_game(const std::filesystem::path& inventory) noexcept {
    try {
        const auto absolute = std::filesystem::absolute(inventory).lexically_normal();
        const auto key = absolute.wstring();
        const auto digest = diagnostics::sha256(std::as_bytes(std::span(key.data(), key.size())));
        if (digest.empty())
            throw std::runtime_error("inventory lock identity unavailable");
        const auto name = L"Local\\unlockall.inventory." + std::wstring(digest.begin(), digest.end());
        unique_handle lock(CreateMutexW(nullptr, FALSE, name.c_str()));
        if (!lock)
            throw std::runtime_error("inventory lock unavailable");
        const auto acquired = WaitForSingleObject(lock.get(), 0);
        if (acquired != WAIT_OBJECT_0 && acquired != WAIT_ABANDONED)
            return 2;

        struct release {
            HANDLE value;

            ~release() { ReleaseMutex(value); }
        } guard{lock.get()};

        std::filesystem::create_directories(absolute.parent_path());
        auto catalog = absolute;
        catalog += L".catalog.json";
        if (sdk::export_inventory_catalog(catalog) != 0)
            return 1;
        generate_inventory(absolute, catalog);
        return 0;
    } catch (const std::exception& error) {
        diagnostics::write("inventory_update_failed", {{"message", error.what()}});
    } catch (...) {
    }
    return 1;
}
}
