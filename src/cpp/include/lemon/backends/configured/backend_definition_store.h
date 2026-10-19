#pragma once

#include "lemon/backends/configured/backend_definition.h"

namespace lemon::backends {

struct InstalledHook {
    std::string id, stage, operation;
};

std::string schema_digest(const std::string& text);
nlohmann::json installed_definition_features();
const std::vector<InstalledHook>& installed_hook_manifest();

class BackendDefinitionStore {
public:
    DefinitionValidationResult validate(const DefinitionSource& source) const;
};

}
