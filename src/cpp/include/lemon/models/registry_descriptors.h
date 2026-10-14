#pragma once

#include "registry_id.h"

namespace lemon {
namespace models {

// The hosts that ship with lemonade; registration is the table's only writer.
namespace builtin_registries {

RegistryDescriptor huggingface();
RegistryDescriptor modelscope();

}  // namespace builtin_registries

}  // namespace models
}  // namespace lemon
