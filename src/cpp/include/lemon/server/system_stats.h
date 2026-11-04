#pragma once

#include <nlohmann/json.hpp>

namespace lemon {

class Server;
class SystemMetricsPlatform;

// GET /system-stats and the job engine's system_stats op report the same reading, so
// both build it here. An unavailable reading is null.
nlohmann::json system_stats_json(Server& server, SystemMetricsPlatform& metrics);

} // namespace lemon
