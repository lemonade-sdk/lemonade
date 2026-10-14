#pragma once

#include "lemon/backends/configured/backend_definition.h"

namespace lemon::backends {

class BackendDefinitionStore {
public:
    DefinitionValidationResult validate(const DefinitionSource& source) const;
};

}
