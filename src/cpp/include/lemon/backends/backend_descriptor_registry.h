#pragma once

#include <string>
#include <vector>
#include "lemon/backends/backend_descriptor.h"

namespace lemon {
namespace backends {

// Read-only view over every backend descriptor (plain data). This API is
// CLI-safe: it pulls in no server classes, so it links into both the lemonade
// CLI and lemond. The factory side (create_server) lives in backend_registry.h
// and is server-only.

// All registered descriptors, in LEMON_BACKENDS order.
const std::vector<const BackendDescriptor*>& all_descriptors();

// Descriptor for a recipe, or nullptr if the recipe has no registered backend.
const BackendDescriptor* descriptor_for(const std::string& recipe);

// True if the recipe is backed by a registered descriptor.
bool has_backend(const std::string& recipe);

} // namespace backends
} // namespace lemon
