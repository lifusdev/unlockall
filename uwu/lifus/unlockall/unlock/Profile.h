#pragma once
#include "lifus/unlockall/qb/Image.h"
#include "lifus/unlockall/qb/Registry.h"
#include <string>

namespace uwu::sdk {
struct inv_proof {
    std::uint32_t begin{}, end{};
    std::string hash;
};

struct inv_profile {
    locators::registry_descriptor registry;
    std::uint64_t entry_table{}, config_table{}, control_table{};
    std::uint32_t member{}, config{}, routes{};
    std::vector<inv_proof> proofs;
};

[[nodiscard]] std::optional<inv_profile> discover_inventory(const locators::code_image& image,
    std::stop_token stop = {}, const locators::validation_observer& observe = {});
[[nodiscard]] inv_profile read_inventory_profile(const memory& process, const module_info& module);
}
