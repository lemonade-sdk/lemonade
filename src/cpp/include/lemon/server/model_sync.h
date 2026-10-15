#pragma once

#include <nlohmann/json.hpp>

namespace lemon {

// POST /internal/models/sync and GET /internal/models/sync/status answer with the same
// status object.
nlohmann::ordered_json model_sync_status_schema();

} // namespace lemon
