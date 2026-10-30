#pragma once
#include "lifus/unlockall/mem/Handle.h"
#include <filesystem>
#include <optional>
#include <chrono>

namespace uwu {
class update_job {
    unique_handle process_;
    unique_handle job_;
    std::chrono::steady_clock::time_point started_;

  public:
    bool start(const std::filesystem::path& inventory);
    bool busy() const noexcept;
    std::optional<DWORD> poll();
    void stop() noexcept;
};
}
