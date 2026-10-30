#include <winsock2.h>
#include <ws2tcpip.h>
#include "lifus/unlockall/unlock/Server.h"
#include "lifus/unlockall/unlock/Inventory.h"
#include "lifus/unlockall/log/Log.h"
#include <atomic>
#include <fstream>
#include <thread>
#include <stdexcept>
#include <array>

namespace uwu {
namespace {
struct socket {
    SOCKET value = INVALID_SOCKET;

    ~socket() {
        if (value != INVALID_SOCKET)
            closesocket(value);
    }
};

bool send(SOCKET socket_value, std::string_view value_data, std::stop_token stop_value) {
    while (!value_data.empty() && !stop_value.stop_requested()) {
        const auto amount = static_cast<int>(std::min<std::size_t>(value_data.size(), 65536));
        const auto sent = ::send(socket_value, value_data.data(), amount, 0);
        if (sent <= 0)
            return false;
        value_data.remove_prefix(static_cast<std::size_t>(sent));
    }
    return value_data.empty();
}
}

struct http_server::impl {
    socket listener;
    bool winsock{};
    std::filesystem::path inventory_path;
    inventory document;
    std::string reload_error;
    std::chrono::steady_clock::time_point next_reload{};
    std::atomic<bool> running{true};
    std::atomic<int> reload_state{};
    std::atomic<std::uint64_t> requests{};
    std::jthread worker;

    ~impl() {
        if (worker.joinable()) {
            worker.request_stop();
            worker.join();
        }
        if (listener.value != INVALID_SOCKET) {
            closesocket(listener.value);
            listener.value = INVALID_SOCKET;
        }
        if (winsock)
            WSACleanup();
    }

    bool reload(bool requested = false, bool validate_value = false) noexcept {
        const auto now = std::chrono::steady_clock::now();
        if (!requested && now < next_reload)
            return true;
        next_reload = now + std::chrono::seconds(1);
        try {
            if (!validate_value && std::filesystem::last_write_time(inventory_path) == document.modified &&
                std::filesystem::file_size(inventory_path) == document.body.size())
                return true;
            auto replacement = load_inventory(inventory_path);
            document = std::move(replacement);
            reload_error.clear();
            diagnostics::write(
                "local_inventory_reloaded", {{"items", document.items}, {"bytes", document.body.size()}});
            return true;
        } catch (const std::exception& error_value) {
            if (reload_error != error_value.what()) {
                reload_error = error_value.what();
                diagnostics::write("local_inventory_reload_rejected",
                    {{"message", reload_error}, {"retained_items", document.items}});
            }
            return false;
        }
    }

    void run(std::stop_token stop_value) noexcept {
        try {
            while (!stop_value.stop_requested()) {
                if (reload_state.load() == 1)
                    reload_state = reload(true, true) ? 2 : 3;
                else
                    reload();
                fd_set ready;
                FD_ZERO(&ready);
                FD_SET(listener.value, &ready);
                timeval timeout{0, 100000};
                const auto available = select(0, &ready, nullptr, nullptr, &timeout);
                if (available == SOCKET_ERROR)
                    break;
                if (!available)
                    continue;
                sockaddr_in peer{};
                int size_value = sizeof(peer);
                socket client{accept(listener.value, reinterpret_cast<sockaddr*>(&peer), &size_value)};
                if (client.value == INVALID_SOCKET)
                    continue;
                if (peer.sin_addr.s_addr != htonl(INADDR_LOOPBACK))
                    continue;
                DWORD timeout_ms = 1000;
                setsockopt(client.value, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms),
                    sizeof(timeout_ms));
                setsockopt(client.value, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout_ms),
                    sizeof(timeout_ms));
                std::string request;
                std::array<char, 2048> buffer{};
                while (request.size() < 16384 && request.find("\r\n\r\n") == std::string::npos &&
                    !stop_value.stop_requested()) {
                    const auto got = recv(client.value, buffer.data(), static_cast<int>(buffer.size()), 0);
                    if (got <= 0)
                        break;
                    request.append(buffer.data(), static_cast<std::size_t>(got));
                }
                const auto first = request.find(' '),
                           second = first == std::string::npos ? first : request.find(' ', first + 1);
                if (request.find("\r\n\r\n") == std::string::npos || first == std::string::npos ||
                    second == std::string::npos)
                    continue;
                const auto method = std::string_view(request).substr(0, first);
                auto path_value = std::string_view(request).substr(first + 1, second - first - 1);
                path_value = path_value.substr(0, path_value.find('?'));
                const bool inventory = method == "GET" && path_value == "/inventory";
                const bool health_value = method == "GET" && path_value == "/health";
                if (inventory || health_value)
                    reload(true);
                const std::string_view response = inventory ? std::string_view(document.body)
                    : health_value                          ? std::string_view(document.health)
                                                            : std::string_view("{}");
                const auto status_value = inventory || health_value ? "200 ok"
                    : method != "GET"                               ? "405 method not allowed"
                                                                    : "404 not found";
                const std::string header_value = std::string("HTTP/1.1 ") + status_value +
                    "\r\ncontent-type: application/json; charset=utf-8\r\ncontent-length: " +
                    std::to_string(response.size()) +
                    "\r\ncache-control: no-store\r\nconnection: close\r\n\r\n";
                if (send(client.value, header_value, stop_value) &&
                    send(client.value, response, stop_value) && inventory) {
                    const auto count_value = ++requests;
                    diagnostics::write("local_inventory_served",
                        {{"requests", count_value}, {"bytes", document.body.size()}});
                }
                shutdown(client.value, SD_SEND);
            }
        } catch (const std::exception& error_value) {
            diagnostics::write("local_inventory_server_error", {{"message", error_value.what()}});
        }
        running = false;
    }
};

http_server::http_server() = default;
http_server::~http_server() = default;

void http_server::request_reload() noexcept {
    if (impl_)
        impl_->reload_state = 1;
}

std::optional<bool> http_server::reload_result() const noexcept {
    if (!running())
        return false;
    const auto state_value = impl_->reload_state.load();
    return state_value >= 2 ? std::optional<bool>(state_value == 2) : std::nullopt;
}

bool http_server::running() const noexcept {
    return impl_ && impl_->running.load();
}

std::uint64_t http_server::requests() const noexcept {
    return impl_ ? impl_->requests.load() : 0;
}

void http_server::stop() noexcept {
    impl_.reset();
}

void http_server::start(const std::filesystem::path& inventory, std::uint16_t port) {
    if (impl_)
        throw std::runtime_error("local inventory server already running");
    auto next = std::make_unique<impl>();
    next->inventory_path = inventory;
    next->document = load_inventory(inventory);
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data))
        throw std::runtime_error("local sockets unavailable");
    next->winsock = true;
    next->listener.value = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (next->listener.value == INVALID_SOCKET)
        throw std::runtime_error("local socket creation failed");
    BOOL exclusive = TRUE;
    if (setsockopt(next->listener.value, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
            reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)))
        throw std::runtime_error("cannot reserve local endpoint");
    sockaddr_in address_value{};
    address_value.sin_family = AF_INET;
    address_value.sin_port = htons(port);
    address_value.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(next->listener.value, reinterpret_cast<sockaddr*>(&address_value), sizeof(address_value)) ||
        listen(next->listener.value, 4))
        throw std::runtime_error("127.0.0.1:8080 is unavailable");
    const auto item_count = next->document.items;
    next->worker =
        std::jthread([server = next.get()](std::stop_token stop_value) { server->run(stop_value); });
    impl_ = std::move(next);
    diagnostics::write(
        "local_inventory_server_ready", {{"items", item_count}, {"port", port}, {"loopback_only", true}});
}
}
