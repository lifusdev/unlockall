#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include "lifus/unlockall/unlock/Reconnect.h"
#include "lifus/unlockall/mem/Handle.h"
#include <array>
#include <filesystem>
#include <vector>
#include <algorithm>
#include <system_error>
#include <cstring>
#include <stdexcept>

namespace uwu {
namespace {
std::vector<std::byte> table(ULONG family) {
    DWORD size_value{};
    auto result_value =
        GetExtendedTcpTable(nullptr, &size_value, FALSE, family, TCP_TABLE_OWNER_PID_CONNECTIONS, 0);
    if (result_value != ERROR_INSUFFICIENT_BUFFER)
        throw std::system_error(result_value, std::system_category(), "tcp table size");
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        if (!size_value || size_value > 8 * 1024 * 1024)
            throw std::runtime_error("tcp table bounds");
        std::vector<std::byte> bytes_value(size_value);
        result_value = GetExtendedTcpTable(
            bytes_value.data(), &size_value, FALSE, family, TCP_TABLE_OWNER_PID_CONNECTIONS, 0);
        if (!result_value)
            return bytes_value;
        if (result_value != ERROR_INSUFFICIENT_BUFFER)
            break;
    }
    throw std::system_error(result_value, std::system_category(), "tcp table read");
}

std::vector<MIB_TCPROW_OWNER_PID> connections(DWORD pid_value) {
    const auto data = table(AF_INET);
    const auto* table_value = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(data.data());
    if (data.size() < offsetof(MIB_TCPTABLE_OWNER_PID, table) ||
        table_value->dwNumEntries >
            (data.size() - offsetof(MIB_TCPTABLE_OWNER_PID, table)) / sizeof(MIB_TCPROW_OWNER_PID))
        throw std::runtime_error("tcp row bounds");
    std::vector<MIB_TCPROW_OWNER_PID> result_value;
    for (DWORD i = 0; i < table_value->dwNumEntries; ++i) {
        const auto& row = table_value->table[i];
        if (row.dwOwningPid == pid_value &&
            (row.dwState == MIB_TCP_STATE_ESTAB || row.dwState == MIB_TCP_STATE_SYN_SENT) &&
            row.dwRemoteAddr && (ntohl(row.dwRemoteAddr) >> 24) != 127)
            result_value.push_back(row);
    }
    return result_value;
}
}

reconnect_result reconnect_game(DWORD pid_value) {
    unique_handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid_value));
    if (!process)
        throw std::system_error(GetLastError(), std::system_category(), "reconnect game lookup");
    std::array<wchar_t, 32768> path_value{};
    DWORD count_value = static_cast<DWORD>(path_value.size());
    if (!QueryFullProcessImageNameW(process.get(), 0, path_value.data(), &count_value) ||
        _wcsicmp(std::filesystem::path(std::wstring_view(path_value.data(), count_value)).filename().c_str(),
            L"rainbowsix.exe"))
        throw std::runtime_error("reconnect target is not rainbowsix.exe");
    reconnect_result result_value;
    const auto ipv6 = table(AF_INET6);
    const auto* table6 = reinterpret_cast<const MIB_TCP6TABLE_OWNER_PID*>(ipv6.data());
    if (ipv6.size() < offsetof(MIB_TCP6TABLE_OWNER_PID, table) ||
        table6->dwNumEntries >
            (ipv6.size() - offsetof(MIB_TCP6TABLE_OWNER_PID, table)) / sizeof(MIB_TCP6ROW_OWNER_PID))
        throw std::runtime_error("ipv6 row bounds");
    for (DWORD i = 0; i < table6->dwNumEntries; ++i) {
        const auto& row = table6->table[i];
        IN6_ADDR remote{};
        std::memcpy(&remote, row.ucRemoteAddr, sizeof(remote));
        if (row.dwOwningPid == pid_value && row.dwState == MIB_TCP_STATE_ESTAB &&
            !IN6_IS_ADDR_LOOPBACK(&remote))
            ++result_value.ipv6;
    }
    const auto initial = connections(pid_value);
    if (initial.size() > 128)
        throw std::runtime_error("too many game connections");
    for (const auto& row : initial) {
        DWORD state_value{};
        if (!GetExitCodeProcess(process.get(), &state_value) || state_value != STILL_ACTIVE)
            break;
        const auto fresh = connections(pid_value);
        if (std::none_of(fresh.begin(), fresh.end(), [&](const auto& value_data) {
                return value_data.dwLocalAddr == row.dwLocalAddr &&
                    value_data.dwLocalPort == row.dwLocalPort &&
                    value_data.dwRemoteAddr == row.dwRemoteAddr &&
                    value_data.dwRemotePort == row.dwRemotePort;
            }))
            continue;
        MIB_TCPROW close_value{};
        close_value.dwState = MIB_TCP_STATE_DELETE_TCB;
        close_value.dwLocalAddr = row.dwLocalAddr;
        close_value.dwLocalPort = row.dwLocalPort;
        close_value.dwRemoteAddr = row.dwRemoteAddr;
        close_value.dwRemotePort = row.dwRemotePort;
        const auto error_value = SetTcpEntry(&close_value);
        if (error_value) {
            ++result_value.failed;
            result_value.error = error_value;
        } else
            ++result_value.closed;
    }
    return result_value;
}
}
