#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace lemon {
namespace backends {
namespace llamacpp {

nlohmann::json sanitize_tool_schema_limits(nlohmann::json request);
nlohmann::json with_max_tokens_alias(nlohmann::json request);
nlohmann::json normalize_response_model(nlohmann::json response, const nlohmann::json& request, const std::string& model_name);

}  // namespace llamacpp
}  // namespace backends
}  // namespace lemon
