#pragma once

#include "lemon/backends/configured/backend_definition.h"

namespace lemon::backends {

std::string schema_digest(const std::string& text);
nlohmann::json installed_definition_features();

class BackendDefinitionStore {
public:
    DefinitionValidationResult validate(const DefinitionSource& source) const;
};

}
