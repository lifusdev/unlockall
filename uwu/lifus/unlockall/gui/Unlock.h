#pragma once
#include "lifus/unlockall/unlock/Server.h"
#include "lifus/unlockall/unlock/Job.h"
#include "lifus/unlockall/unlock/Route.h"
#include <string>
#include <chrono>

namespace uwu {
class unlock {
    http_server server_;
    update_job updater_;
    sdk::inv_route route_;
    bool enabled_ = true;
    std::filesystem::path inventory_;
    DWORD pending_pid_{};
    DWORD active_pid_{};
    bool pending_reload_{};
    std::string status_ = "waiting for game";
    std::chrono::steady_clock::time_point next_check_{};
    void activate(DWORD pid);
    void finish_reconnect(DWORD pid_value);

  public:
    void set_enabled(bool enabled_value, DWORD pid_value);
    void menu(DWORD pid_value);
    void poll(DWORD pid_value);
    void update_and_reconnect(DWORD pid_value);
};
}
