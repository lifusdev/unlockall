#include "lifus/unlockall/unlock/Cfg.h"
#include "lifus/unlockall/gui/Unlock.h"
#include "lifus/unlockall/gui/Ui.h"
#include "lifus/unlockall/log/Log.h"
#include "lifus/unlockall/unlock/Reconnect.h"
#include "lifus/unlockall/mem/Paths.h"
#include <filesystem>
#include <array>
#include <stdexcept>

namespace uwu {
void unlock::set_enabled(bool enabled, DWORD pid) {
    enabled_ = enabled;
    try {
        if (enabled) {
            status_ = pid ? "ready - press unlock all" : "waiting for game";
        } else {
            updater_.stop();
            pending_pid_ = 0;
            pending_reload_ = false;
            route_.restore();
            server_.stop();
            active_pid_ = 0;
            status_ = "off - original route restored";
        }
    } catch (const std::exception& error) {
        if (!enabled && route_.active())
            enabled_ = true;
        status_ = error.what();
        diagnostics::write(
            "local_inventory_action_failed", {{"message", status_}, {"route_active", route_.active()}});
    }
}

void unlock::activate(DWORD pid) {
    if (!server_.running()) {
        server_.stop();
        server_.start(inventory_);
    }
    try {
        if (!route_.active())
            route_.apply(pid, inv::route_url);
        active_pid_ = pid;
    } catch (...) {
        if (!route_.active())
            server_.stop();
        throw;
    }
}

void unlock::menu(DWORD pid) {
    ImGui::BeginDisabled(pending_pid_ != 0);
    if (action_button(pending_pid_ ? "updating..." : "unlock all"))
        update_and_reconnect(pid);
    ImGui::EndDisabled();
}

void unlock::poll(DWORD pid) {
    const auto now = std::chrono::steady_clock::now();
    if (!enabled_ || now < next_check_)
        return;
    next_check_ = now + std::chrono::milliseconds(500);
    if (pending_pid_ &&
        (pending_pid_ != pid || (pending_reload_ && (!route_.active() || !server_.running())))) {
        updater_.stop();
        pending_pid_ = 0;
        pending_reload_ = false;
        diagnostics::write("inventory_reconnect_cancelled");
    }
    if (active_pid_ && active_pid_ != pid) {
        try {
            route_.restore();
            server_.stop();
            active_pid_ = 0;
        } catch (const std::exception& error) {
            status_ = error.what();
            return;
        }
    }
    if (const auto result = updater_.poll(); result && pending_pid_) {
        if (*result == 0) {
            try {
                activate(pid);
                server_.request_reload();
                pending_reload_ = true;
            } catch (const std::exception& error) {
                pending_pid_ = 0;
                status_ = error.what();
                diagnostics::write("inventory_activation_failed", {{"message", status_}});
            }
        } else {
            pending_pid_ = 0;
            status_ = "inventory update failed - reconnect cancelled";
            diagnostics::write("inventory_update_failed", {{"exit_code", *result}});
        }
    }
    if (pending_pid_ && pending_reload_)
        if (const auto result = server_.reload_result()) {
            const auto requested_pid = pending_pid_;
            pending_pid_ = 0;
            pending_reload_ = false;
            if (*result)
                finish_reconnect(requested_pid);
            else {
                status_ = "inventory validation failed - reconnect cancelled";
                diagnostics::write("inventory_reconnect_reload_rejected");
            }
        }
    if (!route_.active())
        return;
    try {
        route_.maintain();
    } catch (const std::exception& error) {
        if (status_ != error.what()) {
            status_ = error.what();
            diagnostics::write("inventory_route_retry", {{"message", status_}});
        }
    }
}

void unlock::update_and_reconnect(DWORD pid) {
    if (pending_pid_)
        return;
    if (!pid || !enabled_ || (active_pid_ && active_pid_ != pid)) {
        status_ = "game is not ready";
        return;
    }
    try {
        inventory_ = paths::inventory();
    } catch (const std::exception& error) {
        status_ = error.what();
        return;
    }
    if (updater_.start(inventory_)) {
        pending_pid_ = pid;
        pending_reload_ = false;
        status_ = "updating inventory before reconnect";
    } else
        status_ = "inventory updater could not start";
}

void unlock::finish_reconnect(DWORD pid) {
    try {
        if (!route_.active() || !server_.running())
            throw std::runtime_error("enable local inventory first");
        route_.maintain();
        const auto result = reconnect_game(pid);
        diagnostics::write("inventory_reconnect",
            {{"closed", result.closed}, {"failed", result.failed}, {"ipv6_remaining", result.ipv6},
                {"win32", result.error}});
        if (result.error)
            status_ = "reconnect failed (win32 " + std::to_string(result.error) + ")";
        else if (!result.closed)
            status_ = "no remote tcp connections to reset";
        else
            status_ = "reset " + std::to_string(result.closed) + " connections - waiting for inventory";
        if (result.ipv6)
            status_ += "; ipv6 sessions remain";
    } catch (const std::exception& error) {
        status_ = error.what();
        diagnostics::write("inventory_reconnect_failed", {{"message", status_}});
    }
}
}
