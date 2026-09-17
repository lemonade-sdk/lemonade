#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace lemonade {
class LemonadeClient;
}

namespace lemon_cli {

using json = nlohmann::json;

// Build a hardware profile JSON from system-info + system-stats.
// Captures OS, CPU, GPU array, RAM, and backend versions for shareable benchmarks.
// `recipes` and `backends` filter which backend versions to include (empty = all).
json build_hardware_profile(const json& sys_info,
                            const std::vector<std::string>& recipes,
                            const std::vector<std::string>& backends,
                            const std::string& lemonade_version);

} // namespace lemon_cli
