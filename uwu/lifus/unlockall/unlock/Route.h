#pragma once
#include <windows.h>
#include <memory>
#include <string_view>

namespace uwu::sdk {
class inv_route {
    struct impl;
    std::unique_ptr<impl> impl_;
    static std::unique_ptr<impl> prepare(DWORD pid_value, bool writable);

  public:
    inv_route();
    ~inv_route();
    void apply(DWORD pid_value, std::string_view local_url);
    void verify(DWORD pid_value);
    void restore();
    void maintain();
    bool active() const noexcept;
};
}
