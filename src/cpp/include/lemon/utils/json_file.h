#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace lemon {
namespace utils {

using json = nlohmann::json;

// Reads a config document, treating a missing or unparseable file as an empty
// object so one damaged file cannot stop the server from starting.
json load_optional_json(const std::string& path);

// Writes through a unique sibling temp file, then renames into place: a reader
// never sees a truncated document, and concurrent writers do not collide on one
// temp path.
void save_json_atomic(const std::string& path, const json& value);

}  // namespace utils
}  // namespace lemon
