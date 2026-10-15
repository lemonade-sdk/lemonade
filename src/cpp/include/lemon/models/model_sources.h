#pragma once

#include <lemon/models/store_snapshot.h>

#include <nlohmann/json.hpp>

namespace lemon {
namespace models {

// Every source fact the docs generator renders, from the published snapshot and the
// compiled descriptor table. Nothing here is derived a second time in prose.
nlohmann::json model_sources_json(const StoreSnapshot& snapshot);

}  // namespace models
}  // namespace lemon
