#pragma once

#include "lemon/backends/configured/backend_definition_composition.h"

namespace lemon::backends {

const ResolvedDefinition& builtin_resolved_source(const DefinitionKey& key);
bool builtin_source_declares_recipe(std::string_view recipe);

}
