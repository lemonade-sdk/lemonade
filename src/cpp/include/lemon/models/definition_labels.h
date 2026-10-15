#pragma once

#include <nlohmann/json.hpp>

#include <set>
#include <string>

namespace lemon {
namespace models {

// Callers report the result verbatim, so the model name is already in it.
std::string describe_illegal_labels(const std::string& model_name, const std::string& reason);

// The one label set registration and routing validation derive a type from. When
// `illegal` is set, the returned labels are meaningless.
std::set<std::string> normalized_definition_labels(const nlohmann::json& model_data,
                                                  std::string* illegal = nullptr);

}  // namespace models
}  // namespace lemon
