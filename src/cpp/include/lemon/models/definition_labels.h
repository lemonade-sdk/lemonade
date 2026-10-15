#pragma once

#include <nlohmann/json.hpp>

#include <set>
#include <string>

namespace lemon {
namespace models {

// An illegal label set names the model it came from, wherever it is reported —
// a /pull 400, a collection import refusal, a skipped entry on load.
std::string describe_illegal_labels(const std::string& model_name, const std::string& reason);

// The label set a definition normalizes to: explicit labels, plus the capability
// booleans, plus the recipe's default deployment mode. Registration and routing
// validation must derive one type from one definition, so this is the only place
// that spelling exists. `illegal`, when given, receives why the definition cannot
// describe a model, so the caller refuses it in those words; the returned set is
// meaningless when it is non-empty.
std::set<std::string> normalized_definition_labels(const nlohmann::json& model_data,
                                                  std::string* illegal = nullptr);

}  // namespace models
}  // namespace lemon
