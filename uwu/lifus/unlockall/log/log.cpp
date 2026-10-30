#include "lifus/unlockall/log/Internal.h"
#include "lifus/unlockall/log/Zip.h"
#include "lifus/unlockall/mem/Handle.h"
#include <bcrypt.h>
#include <shellapi.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <system_error>
#include <thread>
#include <type_traits>
#include <vector>

namespace uwu::diagnostics {
namespace {
using clock = std::chrono::steady_clock;
constexpr std::uint64_t file_limit = 8 * 1024 * 1024;
constexpr std::uint64_t evidence_limit = 1024ull * 1024 * 1024;

std::string quote(std::string_view value_data) {
    std::string out = "\"";
    auto length = std::min<std::size_t>(value_data.size(), 16384);
    if (length < value_data.size())
        while (length && (static_cast<unsigned char>(value_data[length]) & 0xc0) == 0x80)
            --length;
    const auto bounded = value_data.substr(0, length);
    const bool utf8 = bounded.empty() ||
        MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, bounded.data(), static_cast<int>(bounded.size()), nullptr, 0) != 0;
    for (const auto c : bounded) {
        const auto byte = static_cast<unsigned char>(c);
        if (c == '\\' || c == '"') {
            out += '\\';
            out += c;
        } else if (byte < 32 || (!utf8 && byte >= 128)) {
            constexpr char digits[] = "0123456789abcdef";
            out += "\\u00";
            out += digits[byte >> 4];
            out += digits[byte & 15];
        } else
            out += c;
    }
    if (value_data.size() > length)
        out += "[truncated]";
    return out + '"';
}

std::string utc() {
    SYSTEMTIME time{};
    GetSystemTime(&time);
    char text_value[40]{};
    snprintf(text_value, sizeof(text_value), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", time.wYear, time.wMonth,
        time.wDay, time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
    return text_value;
}

struct recorder {
    std::mutex lifecycle;
    std::mutex mutex;
    std::condition_variable_any wake;
    archive archive;
    std::wstring path;
    std::string phase = "application startup";
    float progress{};
    DWORD error{};
    std::uint64_t bytes{}, sequence{}, records_bytes{}, evidence_bytes{};
    bool records_limited{}, evidence_limited{};
    limits limits;
    clock::time_point started = clock::now(), changed = started, updated = started;
    std::jthread monitor;

    ~recorder() { stop(); }

    void stop() noexcept {
        const std::lock_guard lifecycle_lock(lifecycle);
        monitor.request_stop();
        wake.notify_all();
        if (monitor.joinable())
            monitor.join();
        const std::lock_guard lock(mutex);
        if (const auto failure_value = archive.close())
            error = failure_value;
    }
};

recorder& state() {
    static recorder state_value;
    return state_value;
}

void append(recorder& state_value, std::string_view text_value) {
    if (const auto error_value = state_value.archive.append(text_value)) {
        state_value.error = error_value;
        state_value.archive.close();
        return;
    }
    state_value.bytes += text_value.size();
}

std::wstring temporary_directory() {
    std::array<wchar_t, 32768> text_value{};
    const auto length = GetTempPathW(static_cast<DWORD>(text_value.size()), text_value.data());
    if (!length || length >= text_value.size())
        throw std::system_error(ERROR_PATH_NOT_FOUND, std::system_category());
    return (std::filesystem::path(std::wstring(text_value.data(), length)) / L"unlockall" / L"logs").wstring();
}

std::wstring directory() {
    std::array<wchar_t, 32768> text_value{};
    const auto length =
        GetEnvironmentVariableW(L"localappdata", text_value.data(), static_cast<DWORD>(text_value.size()));
    return length && length < text_value.size()
        ? (std::filesystem::path(std::wstring(text_value.data(), length)) / L"unlockall" / L"logs").wstring()
        : temporary_directory();
}

std::string executable_hash() {
    std::array<wchar_t, 32768> name_value{};
    const auto length = GetModuleFileNameW(nullptr, name_value.data(), static_cast<DWORD>(name_value.size()));
    if (!length || length >= name_value.size())
        return {};
    std::ifstream input_value(
        std::filesystem::path(std::wstring(name_value.data(), length)), std::ios::binary);
    if (!input_value)
        return {};
    const auto size_value = std::filesystem::file_size(std::wstring(name_value.data(), length));
    if (size_value > 64 * 1024 * 1024)
        return {};
    std::vector<std::byte> bytes_value(static_cast<std::size_t>(size_value));
    input_value.read(
        reinterpret_cast<char*>(bytes_value.data()), static_cast<std::streamsize>(bytes_value.size()));
    return input_value ? sha256(bytes_value) : std::string{};
}
}

bool initialize(std::wstring_view directory_value, limits limits_value) noexcept {
    try {
        auto& state_value = state();
        const std::lock_guard lifecycle_lock(state_value.lifecycle);
        retention retention_value;
        {
            const std::lock_guard lock(state_value.mutex);
            if (state_value.archive)
                return true;
            if (state_value.monitor.joinable())
                return false;
            auto folder = directory_value.empty() ? directory() : std::wstring(directory_value);
            try {
                std::filesystem::create_directories(folder);
            } catch (...) {
                if (!directory_value.empty())
                    throw;
                folder = temporary_directory();
                std::filesystem::create_directories(folder);
            }
            state_value.path = (std::filesystem::path(folder) / L"unlockall-diagnostic-latest.zip").wstring();
            state_value.error = state_value.archive.open(state_value.path);
            if (state_value.error && directory_value.empty() &&
                state_value.error != ERROR_SHARING_VIOLATION) {
                const auto fallback = temporary_directory();
                std::filesystem::create_directories(fallback);
                folder = fallback;
                state_value.path =
                    (std::filesystem::path(folder) / L"unlockall-diagnostic-latest.zip").wstring();
                state_value.error = state_value.archive.open(state_value.path);
            }
            if (state_value.error) {
                state_value.archive.close();
                return false;
            }
            retention_value = prune(folder);
            state_value.bytes = state_value.sequence = state_value.records_bytes =
                state_value.evidence_bytes = 0;
            state_value.records_limited = state_value.evidence_limited = false;
            state_value.limits.records = std::clamp(limits_value.records, 4096ull, file_limit);
            state_value.limits.evidence = std::clamp(limits_value.evidence, 4096ull, evidence_limit);
            state_value.phase = "application startup";
            state_value.progress = 0;
            state_value.error = ERROR_SUCCESS;
            state_value.started = state_value.changed = state_value.updated = clock::now();
        }
        const auto hash_value = executable_hash();
        write("run_started",
            {{"schema", 2u}, {"executable_sha256", hash_value}, {"compiled", __DATE__ " " __TIME__},
                {"log_limit_bytes", state_value.limits.records},
                {"evidence_limit_bytes", state_value.limits.evidence}, {"contains_native_code", true},
                {"contains_runtime_pointer_values", true}, {"contains_full_heap_dump", false},
                {"contains_player_names", false}, {"uploads_automatically", false}, {"container", "zip"},
                {"retained_runs", 1u}});
        write(
            "diagnostic_retention", {{"removed", retention_value.removed}, {"win32", retention_value.error}});
        memory_status("startup");
        state_value.monitor = std::jthread([&state_value](std::stop_token stop_value) {
            std::unique_lock lock(state_value.mutex);
            while (!stop_value.stop_requested()) {
                state_value.wake.wait_for(lock, stop_value, std::chrono::seconds(5), [] { return false; });
                if (stop_value.stop_requested())
                    break;
                const auto phase_value = state_value.phase;
                const auto progress = state_value.progress;
                const auto now = clock::now();
                const auto phase_ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(now - state_value.changed).count();
                const auto update_ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(now - state_value.updated).count();
                lock.unlock();
                write("heartbeat",
                    {{"phase", phase_value}, {"progress_percent", progress * 100.0f}, {"phase_ms", phase_ms},
                        {"since_update_ms", update_ms}});
                memory_status("heartbeat");
                lock.lock();
                if (const auto error_value = state_value.archive.flush())
                    state_value.error = error_value;
            }
        });
        return true;
    } catch (...) {
        try {
            const std::lock_guard lock(state().mutex);
            state().archive.close();
            state().error = ERROR_WRITE_FAULT;
        } catch (...) {
        }
        return false;
    }
}

void shutdown() noexcept {
    write("run_stopped");
    state().stop();
}

bool enabled() noexcept {
    const std::lock_guard lock(state().mutex);
    return static_cast<bool>(state().archive);
}

DWORD error() noexcept {
    const std::lock_guard lock(state().mutex);
    return state().error;
}

std::wstring path() {
    const std::lock_guard lock(state().mutex);
    return state().path;
}

void open_folder() noexcept {
    try {
        const auto path_value = path();
        if (enabled() && !path_value.empty()) {
            const auto result_value = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open",
                std::filesystem::path(path_value).parent_path().c_str(), nullptr, nullptr, SW_SHOWNORMAL));
            if (result_value > 32)
                return;
        }
        wchar_t message[160]{};
        swprintf_s(message, L"could not open diagnostic logs (win32 %lu)", error());
        MessageBoxW(nullptr, message, L"unlockall", MB_OK | MB_ICONWARNING);
    } catch (...) {
    }
}

namespace {
bool record(std::string_view event, std::initializer_list<field> fields, std::string_view payload,
    bool evidence) noexcept {
    try {
        auto& state_value = state();
        const std::lock_guard lock(state_value.mutex);
        if (!state_value.archive || (evidence ? state_value.evidence_limited : state_value.records_limited))
            return false;
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - state_value.started).count();
        std::string line_value = "{\"utc\":" + quote(utc()) + ",\"elapsed_ms\":" + std::to_string(elapsed) +
            ",\"sequence\":" + std::to_string(state_value.sequence + 1) + ",\"event\":" + quote(event);
        for (const auto& field_value : fields) {
            line_value += ',' + quote(field_value.name) + ':';
            std::visit(
                [&](const auto& value_data) {
                    using value = std::decay_t<decltype(value_data)>;
                    if constexpr (std::is_same_v<value, std::string_view>)
                        line_value += quote(value_data);
                    else if constexpr (std::is_same_v<value, bool>)
                        line_value += value_data ? "true" : "false";
                    else if constexpr (std::is_same_v<value, double>)
                        line_value += std::isfinite(value_data) ? std::to_string(value_data) : "null";
                    else
                        line_value += std::to_string(value_data);
                },
                field_value.value);
        }
        if (!payload.empty())
            line_value += ",\"payload\":\"" + std::string(payload) + '"';
        line_value += "}\n";
        auto& size_value = evidence ? state_value.evidence_bytes : state_value.records_bytes;
        const auto limit = evidence ? state_value.limits.evidence : state_value.limits.records;
        if (size_value + line_value.size() + 1024 > limit) {
            const auto marker =
                "{\"event\":" + quote(evidence ? "evidence_limit_reached" : "log_limit_reached") +
                ",\"sequence\":" + std::to_string(++state_value.sequence) +
                ",\"limit_bytes\":" + std::to_string(limit) +
                ",\"message\":\"restart to create a fresh diagnostic log\"}\n";
            append(state_value, marker);
            size_value += marker.size();
            (evidence ? state_value.evidence_limited : state_value.records_limited) = true;
            state_value.archive.flush();
            return false;
        }
        ++state_value.sequence;
        append(state_value, line_value);
        size_value += line_value.size();
        return static_cast<bool>(state_value.archive);
    } catch (...) {
    }
    return false;
}
}

void write(std::string_view event, std::initializer_list<field> fields) noexcept {
    record(event, fields, {}, false);
}

bool evidence_record(
    std::string_view event, std::initializer_list<field> fields, std::string_view payload) noexcept {
    return record(event, fields, payload, true);
}

void phase(std::string_view name_value, float progress) noexcept {
    try {
        auto& state_value = state();
        const std::lock_guard lock(state_value.mutex);
        if (state_value.phase != name_value) {
            state_value.phase = name_value;
            state_value.changed = clock::now();
        }
        state_value.progress = std::isfinite(progress) ? std::clamp(progress, 0.0f, 1.0f) : 0.0f;
        state_value.updated = clock::now();
    } catch (...) {
    }
}

std::string hex(std::uint64_t value_data) {
    std::array<char, 18> text_value{};
    text_value[0] = '0';
    text_value[1] = 'x';
    const auto end_value =
        std::to_chars(text_value.data() + 2, text_value.data() + text_value.size(), value_data, 16);
    return {text_value.data(), end_value.ptr};
}

std::string sha256(std::span<const std::byte> bytes_value) noexcept {
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE hash_value{};
    try {
        if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
            return {};
        const auto created = BCryptCreateHash(algorithm, &hash_value, nullptr, 0, nullptr, 0, 0);
        if (created < 0) {
            BCryptCloseAlgorithmProvider(algorithm, 0);
            return {};
        }
        bool valid = true;
        for (std::size_t offset = 0; offset < bytes_value.size() && valid;) {
            const auto count_value =
                static_cast<ULONG>(std::min<std::size_t>(bytes_value.size() - offset, 1024 * 1024));
            valid = BCryptHashData(hash_value,
                        reinterpret_cast<PUCHAR>(const_cast<std::byte*>(bytes_value.data() + offset)),
                        count_value, 0) >= 0;
            offset += count_value;
        }
        std::array<UCHAR, 32> digest{};
        valid =
            valid && BCryptFinishHash(hash_value, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
        BCryptDestroyHash(hash_value);
        hash_value = nullptr;
        BCryptCloseAlgorithmProvider(algorithm, 0);
        algorithm = nullptr;
        if (!valid)
            return {};
        constexpr char digits[] = "0123456789abcdef";
        std::string out;
        for (const auto byte : digest) {
            out += digits[byte >> 4];
            out += digits[byte & 15];
        }
        return out;
    } catch (...) {
        if (hash_value)
            BCryptDestroyHash(hash_value);
        if (algorithm)
            BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }
}
}
