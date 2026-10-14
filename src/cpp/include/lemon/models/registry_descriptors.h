#pragma once

#include "registry_id.h"

namespace lemon {
namespace models {

// The hosts that ship with lemonade. A row reaches the descriptor table only
// through registration, so nothing reads these literals without the row.
namespace builtin_registries {

RegistryDescriptor huggingface();
RegistryDescriptor modelscope();

}  // namespace builtin_registries

}  // namespace models
}  // namespace lemon
