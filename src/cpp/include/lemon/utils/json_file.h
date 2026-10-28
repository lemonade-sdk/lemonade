#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace lemon {
namespace utils {

using json = nlohmann::json;

// Reads a config document; a missing or unparseable file reads as an empty object.
json load_optional_json(const std::string& path);

// Writes a unique sibling temp file then renames, so a reader never sees a
// truncated document and concurrent writers never share a temp path.
void save_json_atomic(const std::string& path, const json& value);

}  // namespace utils
}  // namespace lemon
