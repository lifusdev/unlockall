#include "lifus/unlockall/unlock/Cfg.h"
#include "lifus/unlockall/unlock/Route.h"
#include "lifus/unlockall/unlock/Profile.h"
#include "lifus/unlockall/mem/Memory.h"
#include "lifus/unlockall/log/Log.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <set>
#include <stdexcept>
#include <system_error>

namespace uwu::sdk {
namespace {
using address = std::uint64_t;

struct process_pause {
    using action = LONG(NTAPI*)(HANDLE);
    HANDLE process{};
    action resume_function{};
    bool paused{};

    explicit process_pause(HANDLE target_value) : process(target_value) {
        const auto library = GetModuleHandleW(L"ntdll.dll");
        const auto suspend = reinterpret_cast<action>(GetProcAddress(library, "NtSuspendProcess"));
        resume_function = reinterpret_cast<action>(GetProcAddress(library, "NtResumeProcess"));
        if (!suspend || !resume_function || suspend(process) < 0)
            throw std::runtime_error("cannot pause the game for a consistent data update");
        paused = true;
    }

    ~process_pause() {
        if (paused)
            resume_function(process);
    }

    void resume() {
        if (paused && resume_function(process) < 0)
            throw std::runtime_error("game resume failed");
        paused = false;
    }
};
}

struct inv_route::impl {
    unique_handle process;
    memory memory;
    module_info module;
    inv_profile profile;
    address scheduler{}, first{}, last{}, capacity{}, entry_slot{}, entry{}, owner{}, config{}, tree{};
    address node{}, text_address{}, data{}, control{};
    std::array<address, 4> header{};
    std::vector<std::byte> original, replacement;
    bool active{};

    std::vector<std::byte> bytes(address address_value, std::size_t count_value) const {
        if (address_value < 0x10000 || address_value >= 0x800000000000ull || count_value > 0x100000 ||
            count_value > 0x800000000000ull - address_value)
            throw std::runtime_error("inventory address bounds");
        std::vector<std::byte> result_value(count_value);
        const auto read_value = memory.read_bytes(address_value, result_value);
        if (!read_value.complete())
            throw std::system_error(read_value.error, std::system_category(), "inventory data read");
        return result_value;
    }

    template <class t> t read(address address_value) const {
        const auto bytes_value = bytes(address_value, sizeof(t));
        t value_data;
        std::memcpy(&value_data, bytes_value.data(), sizeof(t));
        return value_data;
    }

    std::string text(address address_value) const {
        const auto value_data = read<std::array<address, 4>>(address_value);
        if (value_data[2] > 512 || value_data[2] > value_data[3] || value_data[3] > 0x100000)
            throw std::runtime_error("inventory string bounds");
        const auto bytes_value = bytes(
            value_data[3] < 16 ? address_value : value_data[0], static_cast<std::size_t>(value_data[2] + 1));
        if (bytes_value.back() != std::byte{})
            throw std::runtime_error("inventory string terminator");
        return {reinterpret_cast<const char*>(bytes_value.data()), bytes_value.size() - 1};
    }

    address find(address sentinel) const {
        address current_value = read<address>(sentinel + 8);
        std::set<address> seen;
        constexpr std::string_view key_value = "profilesInventory";
        while (current_value != sentinel && !read<std::uint8_t>(current_value + 0x19)) {
            if (!seen.insert(current_value).second || seen.size() > 128)
                throw std::runtime_error("inventory route tree bounds");
            const auto name_value = text(read<address>(current_value + 0x20));
            if (name_value == key_value)
                return current_value;
            const auto comparison = _stricmp(name_value.c_str(), key_value.data());
            current_value = read<address>(current_value + (comparison > 0 ? 0 : 16));
        }
        return 0;
    }

    void qualify() const {
        if (profile.proofs.empty())
            throw std::runtime_error("inventory native proofs missing");
        for (const auto& proof_value : profile.proofs)
            if (proof_value.begin >= proof_value.end || proof_value.end > module.size ||
                proof_value.hash.size() != 64 ||
                diagnostics::sha256(bytes(module.address + proof_value.begin,
                    proof_value.end - proof_value.begin)) != proof_value.hash)
                throw std::runtime_error("inventory native code changed");
    }

    address registry_owner() const {
        emulator cpu(memory);
        if (!cpu.run(profile.registry.owner.code).complete())
            throw std::runtime_error("inventory registry recipe failed");
        return cpu.get(profile.registry.owner.output);
    }

    void locate() {
        scheduler = registry_owner();
        first = read<address>(scheduler + profile.registry.first);
        last = read<address>(scheduler + profile.registry.last);
        capacity = read<address>(scheduler + profile.registry.capacity);
        if (last < first || capacity < last || capacity - first > 1024 * 8 || (last - first) % 8 ||
            (capacity - first) % 8)
            throw std::runtime_error("inventory registry bounds");
        for (auto at = first; at < last; at += 8) {
            const auto candidate = read<address>(at);
            if (candidate < 0x10000 + profile.member)
                continue;
            const auto parent = candidate - profile.member;
            if (read<address>(candidate) != profile.entry_table ||
                read<address>(parent + profile.config) != profile.config_table)
                continue;
            const auto map = read<address>(parent + profile.config + profile.routes);
            const auto found = find(map);
            if (!found)
                continue;
            if (node && node != found)
                throw std::runtime_error("multiple inventory route owners");
            entry_slot = at;
            entry = candidate;
            owner = parent;
            config = parent + profile.config;
            tree = map;
            node = found;
        }
        if (!node)
            throw std::runtime_error("inventory route is not initialized");
        text_address = read<address>(node + 0x40);
        control = read<address>(node + 0x48);
        header = read<std::array<address, 4>>(text_address);
        constexpr std::string_view expected =
            "https://{env}public-ubiservices.ubi.com/{version}/profiles/{profileId}/inventory";
        if (text(text_address) != expected || header[3] < 16 || text_address != control + 0x10 ||
            read<address>(control) != profile.control_table)
            throw std::runtime_error("unexpected inventory route layout");
        data = header[0];
        original = bytes(data, static_cast<std::size_t>(header[2] + 1));
        if (original.back() != std::byte{} || original.size() != expected.size() + 1 ||
            std::memcmp(original.data(), expected.data(), expected.size()))
            throw std::runtime_error("inventory route changed during lookup");
    }

    void validate() const {
        if (registry_owner() != scheduler || read<address>(scheduler + profile.registry.first) != first ||
            read<address>(scheduler + profile.registry.last) != last ||
            read<address>(scheduler + profile.registry.capacity) != capacity ||
            read<address>(entry_slot) != entry || entry - profile.member != owner ||
            read<address>(entry) != profile.entry_table || read<address>(config) != profile.config_table ||
            read<address>(config + profile.routes) != tree || find(tree) != node ||
            read<address>(node + 0x40) != text_address || read<address>(node + 0x48) != control ||
            read<address>(control) != profile.control_table || read<std::int32_t>(control + 8) <= 0 ||
            read<std::array<address, 4>>(text_address) != header)
            throw std::runtime_error("inventory route ownership changed");
    }

    void write(std::span<const std::byte> bytes_value) const {
        MEMORY_BASIC_INFORMATION region_value{};
        if (!VirtualQueryEx(
                process.get(), reinterpret_cast<void*>(data), &region_value, sizeof(region_value)) ||
            region_value.State != MEM_COMMIT || (region_value.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
            (region_value.Protect & 0xff) != PAGE_READWRITE ||
            bytes_value.size() >
                reinterpret_cast<address>(region_value.BaseAddress) + region_value.RegionSize - data)
            throw std::runtime_error("inventory buffer is not writable data");
        if (const auto error_value = memory.write_bytes(data, bytes_value))
            throw std::system_error(error_value, std::system_category(), "inventory data write");
    }
};

inv_route::inv_route() = default;

inv_route::~inv_route() {
    try {
        restore();
    } catch (const std::exception& e) {
        diagnostics::write("inventory_restore_failed", {{"reason", e.what()}});
    }
}

bool inv_route::active() const noexcept {
    return impl_ && impl_->active;
}

std::unique_ptr<inv_route::impl> inv_route::prepare(DWORD pid_value, bool writable) {
    auto next = std::make_unique<impl>();
    const DWORD access = PROCESS_QUERY_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION |
        (writable ? PROCESS_SUSPEND_RESUME : 0);
    next->process.reset(OpenProcess(access, FALSE, pid_value));
    if (!next->process)
        throw std::system_error(GetLastError(), std::system_category(), "game data access");
    if (const auto error_value = next->memory.attach(pid_value))
        throw std::system_error(error_value, std::system_category(), "game lookup transport");
    DWORD error_value{};
    const auto modules_value = next->memory.modules(error_value);
    const auto found = std::find_if(modules_value.begin(), modules_value.end(), [](const auto& m) {
        return !_wcsicmp(m.name.c_str(), L"rainbowsix.exe") ||
            !_wcsicmp(m.name.c_str(), L"rainbowsix_vulkan.exe");
    });
    if (error_value || found == modules_value.end())
        throw std::runtime_error("game module not found");
    next->module = *found;
    next->profile = read_inventory_profile(next->memory, next->module);
    next->qualify();
    next->locate();
    next->validate();
    next->qualify();
    return next;
}

void inv_route::verify(DWORD pid_value) {
    if (active())
        throw std::runtime_error("inventory route already active");
    static_cast<void>(prepare(pid_value, false));
}

void inv_route::apply(DWORD pid_value, std::string_view local_url) {
    if (active())
        throw std::runtime_error("inventory route already active");
    if (local_url != inv::route_url)
        throw std::runtime_error("only the local inventory endpoint is allowed");
    auto next = prepare(pid_value, true);
    std::string url(local_url);
    url += '?';
    if (url.size() > next->header[2])
        throw std::runtime_error("inventory url buffer is too short");
    url.resize(static_cast<std::size_t>(next->header[2]), '_');
    url.push_back('\0');
    const auto bytes_value = std::as_bytes(std::span(url.data(), url.size()));
    next->replacement.assign(bytes_value.begin(), bytes_value.end());
    process_pause pause(next->process.get());
    next->qualify();
    next->validate();
    if (next->bytes(next->data, next->original.size()) != next->original)
        throw std::runtime_error("inventory url changed before update");
    next->active = true;
    impl_ = std::move(next);
    try {
        impl_->write(impl_->replacement);
        if (impl_->bytes(impl_->data, impl_->replacement.size()) != impl_->replacement)
            throw std::runtime_error("inventory url verification failed");
    } catch (...) {
        if (impl_->bytes(impl_->data, impl_->original.size()) != impl_->original)
            impl_->write(impl_->original);
        impl_->active = impl_->bytes(impl_->data, impl_->original.size()) != impl_->original;
        throw;
    }
    pause.resume();
    diagnostics::write("inventory_route_enabled",
        {{"route", "profilesinventory"}, {"bytes", impl_->replacement.size()}, {"external_data_only", true}});
}

void inv_route::restore() {
    if (!active())
        return;
    DWORD status_value{};
    if (GetExitCodeProcess(impl_->process.get(), &status_value) && status_value != STILL_ACTIVE) {
        impl_.reset();
        return;
    }
    process_pause pause(impl_->process.get());
    impl_->qualify();
    impl_->validate();
    const auto current_value = impl_->bytes(impl_->data, impl_->original.size());
    if (current_value != impl_->replacement && current_value != impl_->original)
        throw std::runtime_error("inventory url changed; refusing to overwrite it");
    if (current_value != impl_->original)
        impl_->write(impl_->original);
    if (impl_->bytes(impl_->data, impl_->original.size()) != impl_->original)
        throw std::runtime_error("inventory restore verification failed");
    impl_->active = false;
    pause.resume();
    impl_.reset();
    diagnostics::write("inventory_route_restored");
}

void inv_route::maintain() {
    if (!active())
        return;
    bool owned{};
    try {
        impl_->validate();
        owned = true;
    } catch (const std::exception&) {
    }
    if (owned) {
        const auto current_value = impl_->bytes(impl_->data, impl_->original.size());
        if (current_value == impl_->replacement)
            return;
        if (current_value != impl_->original)
            throw std::runtime_error("inventory route changed unexpectedly");
        process_pause pause(impl_->process.get());
        impl_->qualify();
        impl_->validate();
        if (impl_->bytes(impl_->data, impl_->original.size()) != impl_->original)
            throw std::runtime_error("inventory route changed before refresh");
        impl_->write(impl_->replacement);
        if (impl_->bytes(impl_->data, impl_->replacement.size()) != impl_->replacement)
            throw std::runtime_error("inventory route refresh failed");
        pause.resume();
        diagnostics::write("inventory_route_refreshed");
        return;
    }
    auto previous = std::move(impl_);
    try {
        apply(previous->memory.pid(), inv::route_url);
    } catch (...) {
        impl_ = std::move(previous);
        throw;
    }
    diagnostics::write("inventory_route_rebound");
}
}
