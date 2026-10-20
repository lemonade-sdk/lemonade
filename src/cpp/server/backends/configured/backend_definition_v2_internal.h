#pragma once

#include "lemon/backends/configured/backend_definition_resolver.h"

namespace lemon::backends::v2 {

const nlohmann::json& schema_document();
CompositionDiagnostics check_schema_keywords(const nlohmann::json& schema);
std::string pointer_token(std::string_view token);

}
