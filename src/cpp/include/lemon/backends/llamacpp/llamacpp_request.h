#pragma once

#include <nlohmann/json.hpp>

namespace lemon {
namespace backends {
namespace llamacpp {

nlohmann::json sanitize_tool_schema_limits(nlohmann::json request);

}  // namespace llamacpp
}  // namespace backends
}  // namespace lemon
