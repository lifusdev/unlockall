#include "lifus/unlockall/mem/Memory.h"
#include <winioctl.h>
#include <tlhelp32.h>
#include <algorithm>
#include <limits>

namespace uwu {
namespace {

constexpr DWORD ctl_read = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS);
constexpr DWORD ctl_write = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS);
constexpr DWORD ctl_threads = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x805, METHOD_BUFFERED, FILE_ANY_ACCESS);

constexpr std::size_t max_chunk = 1024 * 1024;

#pragma pack(push, 8)

struct memory_request {
    ULONG pid;
    ULONG64 addr;
    ULONG64 buf;
    SIZE_T size;
    BOOLEAN write;
    LONG status;
};

struct thread_record {
    ULONG tid;
    ULONG64 teb;
};

#pragma pack(pop)

DWORD nt_status_error(LONG status_value) {
    using fn = ULONG(NTAPI*)(LONG);
    static const auto convert =
        reinterpret_cast<fn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlNtStatusToDosError"));
    return convert ? convert(status_value) : ERROR_GEN_FAILURE;
}

unique_handle open_driver() {
    return unique_handle(
        CreateFileW(L"\\\\.\\lifus", GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr));
}

DWORD read_transport(HANDLE h, ULONG pid_value, std::uintptr_t addr, void* out, std::size_t n) {
    memory_request r{};
    r.pid = pid_value;
    r.addr = addr;
    r.buf = reinterpret_cast<ULONG64>(out);
    r.size = n;
    DWORD bytes_value = 0;
    if (!DeviceIoControl(h, ctl_read, &r, sizeof(r), &r, sizeof(r), &bytes_value, nullptr))
        return GetLastError();
    if (r.status < 0)
        return nt_status_error(r.status);
    return ERROR_SUCCESS;
}

std::uintptr_t first_teb(HANDLE h, DWORD pid_value, DWORD& error_value) {
    constexpr ULONG cap = 16;
    constexpr DWORD hdr = 2 * sizeof(ULONG);
    constexpr DWORD buf_size = hdr + cap * sizeof(thread_record);
    std::byte raw[buf_size]{};
    auto* fields = reinterpret_cast<ULONG*>(raw);
    fields[0] = pid_value;
    fields[1] = cap;
    DWORD bytes_value = 0;
    if (!DeviceIoControl(h, ctl_threads, raw, buf_size, raw, buf_size, &bytes_value, nullptr)) {
        error_value = GetLastError();
        return 0;
    }
    if (!fields[1]) {
        error_value = ERROR_NOT_FOUND;
        return 0;
    }
    auto* t_value = reinterpret_cast<const thread_record*>(raw + hdr);
    error_value = ERROR_SUCCESS;
    return static_cast<std::uintptr_t>(t_value[0].teb);
}
}

std::wstring error_text(DWORD error_value) {
    if (error_value == ERROR_SUCCESS)
        return L"success";
    wchar_t* message = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
        error_value, 0, reinterpret_cast<LPWSTR>(&message), 0, nullptr);
    std::wstring result_value =
        length && message ? std::wstring(message, length) : L"win32 error " + std::to_wstring(error_value);
    if (message)
        LocalFree(message);
    while (!result_value.empty() && (result_value.back() == L'\r' || result_value.back() == L'\n'))
        result_value.pop_back();
    return result_value;
}

std::vector<process_info> processes(DWORD& error_value) {
    std::vector<process_info> result_value;
    unique_handle snap(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snap) {
        error_value = GetLastError();
        return result_value;
    }
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (!Process32FirstW(snap.get(), &pe)) {
        error_value = GetLastError();
        return result_value;
    }
    do {
        result_value.push_back({pe.th32ProcessID, pe.szExeFile});
    } while (Process32NextW(snap.get(), &pe));
    error_value = ERROR_SUCCESS;
    return result_value;
}

std::optional<DWORD> game_pid() {
    DWORD error{};
    const auto list = processes(error);
    if (error)
        return std::nullopt;
    const auto match = [](const process_info& value) {
        return _wcsicmp(value.name.c_str(), L"rainbowsix.exe") == 0 ||
            _wcsicmp(value.name.c_str(), L"rainbowsix_vulkan.exe") == 0;
    };
    if (std::count_if(list.begin(), list.end(), match) != 1)
        return std::nullopt;
    return std::find_if(list.begin(), list.end(), match)->pid;
}

DWORD memory::attach(DWORD pid_value) noexcept {
    detach();
    statistics_ = {};
    auto drv = open_driver();
    if (!drv)
        return GetLastError();
    driver_ = std::move(drv);
    pid_ = pid_value;
    return ERROR_SUCCESS;
}

read_result memory::read_bytes(std::uintptr_t address_value, std::span<std::byte> output) const noexcept {
    ++statistics_.calls;
    statistics_.requested += output.size();
    const auto finish = [&](read_result result_value) {
        statistics_.transferred += result_value.transferred;
        if (result_value.error) {
            ++statistics_.failures;
            statistics_.last_error = result_value.error;
            for (auto& [error, count] : statistics_.errors)
                if (!count || error == result_value.error) {
                    error = result_value.error;
                    ++count;
                    break;
                }
        }
        return result_value;
    };
    if (!driver_)
        return finish({0, ERROR_INVALID_HANDLE});
    if (output.empty())
        return {};
    if (address_value > std::numeric_limits<std::uintptr_t>::max() - (output.size() - 1))
        return finish({0, ERROR_ARITHMETIC_OVERFLOW});
    std::size_t done = 0;
    while (done < output.size()) {
        const auto n = std::min(max_chunk, output.size() - done);
        const auto err = read_transport(driver_.get(), pid_, address_value + done, output.data() + done, n);
        if (err)
            return finish({done, err});
        done += n;
    }
    return finish({done, ERROR_SUCCESS});
}

DWORD memory::write_bytes(
    std::uintptr_t address_value, std::span<const std::byte> input_value) const noexcept {
    if (!driver_)
        return ERROR_INVALID_HANDLE;
    if (input_value.empty() || input_value.size() > 4096 || address_value < 0x10000 ||
        address_value >= 0x800000000000ull || input_value.size() > 0x800000000000ull - address_value)
        return ERROR_INVALID_PARAMETER;
    std::array<std::byte, 4096> transfer{};
    std::copy(input_value.begin(), input_value.end(), transfer.begin());
    memory_request request{};
    request.pid = pid_;
    request.addr = address_value;
    request.buf = reinterpret_cast<ULONG64>(transfer.data());
    request.size = input_value.size();
    request.write = TRUE;
    DWORD returned{};
    if (!DeviceIoControl(driver_.get(), ctl_write, &request, sizeof(request), &request, sizeof(request),
            &returned, nullptr))
        return GetLastError();
    if (request.status < 0)
        return nt_status_error(request.status);
    if (returned != sizeof(request) || request.pid != pid_ || request.addr != address_value ||
        request.size != input_value.size() || !request.write)
        return ERROR_INVALID_DATA;
    if (!std::equal(input_value.begin(), input_value.end(), transfer.begin()))
        return ERROR_WRITE_FAULT;
    return ERROR_SUCCESS;
}

std::vector<module_info> memory::modules(DWORD& error_value) const {
    std::vector<module_info> result_value;
    if (!driver_) {
        error_value = ERROR_INVALID_HANDLE;
        return result_value;
    }
    const auto teb = first_teb(driver_.get(), pid_, error_value);
    if (!teb)
        return result_value;
    std::uintptr_t peb = 0;
    auto err = read_transport(driver_.get(), pid_, teb + 0x60, &peb, sizeof(peb));
    if (err || !peb) {
        error_value = err ? err : ERROR_BAD_FORMAT;
        return result_value;
    }
    std::uintptr_t ldr = 0;
    err = read_transport(driver_.get(), pid_, peb + 0x18, &ldr, sizeof(ldr));
    if (err || !ldr) {
        error_value = err ? err : ERROR_BAD_FORMAT;
        return result_value;
    }
    const auto head = ldr + 0x10;
    std::uintptr_t entry_value = 0;
    err = read_transport(driver_.get(), pid_, head, &entry_value, sizeof(entry_value));
    if (err || !entry_value) {
        error_value = err ? err : ERROR_BAD_FORMAT;
        return result_value;
    }
    for (unsigned i = 0; i < 512 && entry_value && entry_value != head; ++i) {
        std::byte raw[0x68]{};
        if (read_transport(driver_.get(), pid_, entry_value, raw, sizeof(raw)))
            break;
        const auto base = *reinterpret_cast<std::uintptr_t*>(raw + 0x30);
        const auto size_value = *reinterpret_cast<ULONG*>(raw + 0x40);
        const auto len = *reinterpret_cast<USHORT*>(raw + 0x58);
        const auto buf = *reinterpret_cast<std::uintptr_t*>(raw + 0x60);
        if (!base)
            break;
        std::wstring name_value;
        if (len > 0 && len <= 520 && buf) {
            name_value.resize(len / sizeof(wchar_t));
            if (read_transport(driver_.get(), pid_, buf, name_value.data(), len))
                name_value.clear();
        }
        if (!name_value.empty())
            result_value.push_back({std::move(name_value), base, static_cast<std::size_t>(size_value)});
        const auto next = *reinterpret_cast<std::uintptr_t*>(raw);
        if (!next || next == entry_value)
            break;
        entry_value = next;
    }
    error_value = result_value.empty() ? ERROR_NOT_FOUND : ERROR_SUCCESS;
    return result_value;
}

std::optional<bool> memory::is64_bit(DWORD& error_value) const noexcept {
    if (!driver_) {
        error_value = ERROR_INVALID_HANDLE;
        return std::nullopt;
    }
    const auto teb = first_teb(driver_.get(), pid_, error_value);
    if (!teb)
        return std::nullopt;
    std::uintptr_t peb = 0;
    if (read_transport(driver_.get(), pid_, teb + 0x60, &peb, sizeof(peb)) || !peb) {
        error_value = ERROR_BAD_FORMAT;
        return std::nullopt;
    }
    std::uintptr_t image_base = 0;
    if (read_transport(driver_.get(), pid_, peb + 0x10, &image_base, sizeof(image_base)) || !image_base) {
        error_value = ERROR_BAD_FORMAT;
        return std::nullopt;
    }
    IMAGE_DOS_HEADER dos{};
    if (read_transport(driver_.get(), pid_, image_base, &dos, sizeof(dos)) ||
        dos.e_magic != IMAGE_DOS_SIGNATURE) {
        error_value = ERROR_BAD_FORMAT;
        return std::nullopt;
    }

    struct {
        DWORD sig;
        WORD machine;
    } nt{};

    if (read_transport(driver_.get(), pid_, image_base + dos.e_lfanew, &nt, sizeof(nt)) ||
        nt.sig != IMAGE_NT_SIGNATURE) {
        error_value = ERROR_BAD_FORMAT;
        return std::nullopt;
    }
    if (nt.machine != IMAGE_FILE_MACHINE_AMD64 && nt.machine != IMAGE_FILE_MACHINE_I386) {
        error_value = ERROR_NOT_SUPPORTED;
        return std::nullopt;
    }
    error_value = ERROR_SUCCESS;
    return nt.machine == IMAGE_FILE_MACHINE_AMD64;
}

std::vector<thread_entry> memory::threads(DWORD& error_value) const {
    std::vector<thread_entry> result_value;
    if (!driver_) {
        error_value = ERROR_INVALID_HANDLE;
        return result_value;
    }
    constexpr ULONG cap = 2048;
    constexpr DWORD hdr = 2 * sizeof(ULONG);
    constexpr DWORD buf_size = hdr + cap * sizeof(thread_record);
    std::vector<std::byte> raw(buf_size, std::byte{0});
    auto* fields = reinterpret_cast<ULONG*>(raw.data());
    fields[0] = pid_;
    fields[1] = cap;
    DWORD bytes_value = 0;
    if (!DeviceIoControl(
            driver_.get(), ctl_threads, raw.data(), buf_size, raw.data(), buf_size, &bytes_value, nullptr)) {
        error_value = GetLastError();
        return result_value;
    }
    const auto count_value = fields[1];
    auto* entries = reinterpret_cast<const thread_record*>(raw.data() + hdr);
    for (ULONG i = 0; i < count_value; ++i)
        result_value.push_back({entries[i].tid, static_cast<std::uintptr_t>(entries[i].teb)});
    error_value = ERROR_SUCCESS;
    return result_value;
}
}
