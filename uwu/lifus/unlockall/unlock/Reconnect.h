#pragma once
#include <windows.h>

namespace uwu {
struct reconnect_result {
    unsigned closed{}, failed{}, ipv6{};
    DWORD error{};
};

reconnect_result reconnect_game(DWORD pid_value);
}
