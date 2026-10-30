#pragma once
#include "lifus/unlockall/unlock/Cfg.h"
#include <filesystem>
#include <memory>
#include <cstdint>
#include <optional>

namespace uwu {
class http_server {
    struct impl;
    std::unique_ptr<impl> impl_;

  public:
    http_server();
    ~http_server();
    void start(const std::filesystem::path& inventory, std::uint16_t port = inv::port);
    void stop() noexcept;
    bool running() const noexcept;
    std::uint64_t requests() const noexcept;
    void request_reload() noexcept;
    std::optional<bool> reload_result() const noexcept;
};
}
