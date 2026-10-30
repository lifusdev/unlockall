#include "lifus/unlockall/log/Log.h"
#include <psapi.h>
#include <new>
#include <stdexcept>
#include <system_error>
#include <typeinfo>

namespace uwu::diagnostics {
void memory_status(std::string_view reason) noexcept {
    MEMORYSTATUSEX memory_value{};
    memory_value.dwLength = sizeof(memory_value);
    PERFORMANCE_INFORMATION performance{};
    performance.cb = sizeof(performance);
    PROCESS_MEMORY_COUNTERS_EX process{};
    process.cb = sizeof(process);
    const auto physical_ok = GlobalMemoryStatusEx(&memory_value) != 0;
    const auto commit_ok = GetPerformanceInfo(&performance, sizeof(performance)) != 0;
    const auto process_ok = GetProcessMemoryInfo(GetCurrentProcess(),
                                reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&process), sizeof(process)) != 0;
    write("memory_status",
        {{"reason", reason}, {"physical_available", physical_ok ? memory_value.ullAvailPhys : 0},
            {"physical_total", physical_ok ? memory_value.ullTotalPhys : 0},
            {"commit_bytes", commit_ok ? performance.CommitTotal * performance.PageSize : 0},
            {"commit_limit", commit_ok ? performance.CommitLimit * performance.PageSize : 0},
            {"process_private_bytes", process_ok ? process.PrivateUsage : 0},
            {"process_working_set", process_ok ? process.WorkingSetSize : 0},
            {"physical_available_known", physical_ok}, {"commit_known", commit_ok},
            {"process_known", process_ok}});
}

void exception(std::string_view stage, const std::exception& error_value) noexcept {
    const char* category = dynamic_cast<const std::bad_alloc*>(&error_value) ? "allocation failure"
        : dynamic_cast<const std::length_error*>(&error_value)               ? "container length error"
        : dynamic_cast<const std::out_of_range*>(&error_value)               ? "out of range"
        : dynamic_cast<const std::invalid_argument*>(&error_value)           ? "invalid argument"
        : dynamic_cast<const std::system_error*>(&error_value)               ? "system error"
                                                                             : "standard exception";
    write("exception",
        {{"stage", stage}, {"category", category}, {"type", typeid(error_value).name()},
            {"message", error_value.what()}});
    memory_status("exception");
}
}
