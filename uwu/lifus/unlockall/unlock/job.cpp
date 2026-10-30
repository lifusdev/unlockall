#include "lifus/unlockall/unlock/Job.h"
#include "lifus/unlockall/log/Log.h"
#include "lifus/unlockall/mem/Paths.h"
#include <array>
#include <stdexcept>

namespace uwu {
void update_job::stop() noexcept {
    job_.reset();
    process_.reset();
}

bool update_job::busy() const noexcept {
    return process_ && WaitForSingleObject(process_.get(), 0) == WAIT_TIMEOUT;
}

std::optional<DWORD> update_job::poll() {
    if (!process_)
        return std::nullopt;
    if (busy()) {
        if (std::chrono::steady_clock::now() - started_ < std::chrono::seconds(60))
            return std::nullopt;
        stop();
        return ERROR_TIMEOUT;
    }
    DWORD result{};
    if (!GetExitCodeProcess(process_.get(), &result))
        result = ERROR_GEN_FAILURE;
    stop();
    return result;
}

bool update_job::start(const std::filesystem::path& inventory) {
    if (busy())
        return false;
    stop();
    try {
        const auto executable = paths::executable();
        const auto quoted = [](const std::filesystem::path& path) { return L"\"" + path.wstring() + L"\""; };
        std::wstring command = quoted(executable) + L" --update-inventory " + quoted(inventory);
        unique_handle job(CreateJobObjectW(nullptr, nullptr));
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job ||
            !SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
            throw std::runtime_error("inventory updater lifetime guard unavailable");
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                CREATE_NO_WINDOW | CREATE_SUSPENDED | BELOW_NORMAL_PRIORITY_CLASS, nullptr,
                executable.parent_path().c_str(), &startup, &process))
            throw std::runtime_error("inventory updater launch failed");
        unique_handle child(process.hProcess), thread(process.hThread);
        if (!AssignProcessToJobObject(job.get(), child.get()) || ResumeThread(thread.get()) == DWORD(-1)) {
            TerminateProcess(child.get(), 1);
            throw std::runtime_error("inventory updater lifetime binding failed");
        }
        job_ = std::move(job);
        process_ = std::move(child);
        started_ = std::chrono::steady_clock::now();
        diagnostics::write("inventory_updater_started", {{"on_demand", true}});
        return true;
    } catch (const std::exception& error) {
        diagnostics::write("inventory_updater_start_failed", {{"message", error.what()}});
    }
    return false;
}
}
