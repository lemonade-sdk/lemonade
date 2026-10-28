#pragma once

#include <lemon/models/store_snapshot.h>

#include <nlohmann/json.hpp>

namespace lemon {
namespace models {

// The one place docs may read source facts from.
nlohmann::json model_sources_json(const StoreSnapshot& snapshot);

}  // namespace models
}  // namespace lemon
