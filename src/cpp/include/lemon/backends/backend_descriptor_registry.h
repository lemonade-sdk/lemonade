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

// True if the recipe publishes ROCm release channels (stable/nightly) — i.e. its
// "rocm" backend resolves to a channel-specific artifact. False for recipes whose
// rocm build is a single artifact (or that have no rocm build at all).
bool recipe_has_rocm_channels(const std::string& recipe);

// Every deployment mode the recipe's backend can serve, most-default first.
// Empty for recipes with no registered descriptor (collections, unknown recipes).
const std::vector<std::string>& supported_modes_for(const std::string& recipe);

// The deployment label a model of this recipe carries when its own labels name
// no deployment mode. Recipes with no registered descriptor (omni collections,
// unknown recipes) route through /chat/completions.
const std::string& default_mode_for(const std::string& recipe);

// True if the recipe's backend can serve `mode`. Recipes with no registered
// descriptor accept any mode: their routing is decided by the collection, not
// by a backend that could reject the request.
bool backend_serves_mode(const std::string& recipe, ModelType mode);

// The display string the generated docs and /system-info render for a backend
// (e.g. chat -> "Text generation"), derived from its default mode.
std::string modality_display_for(const BackendDescriptor& descriptor);

// Drop any label claiming a deployment mode `recipe` cannot serve, then stamp
// the recipe's default mode if none is left, plus the backend's default
// capabilities. Every model ends up carrying exactly one mode label, so
// consumers can test for a label instead of inferring the mode from which
// labels happen to be absent.
//
// Returns the labels dropped. Registration treats a non-empty result as a bad
// definition and refuses it; loading a persisted entry accepts the drop, so an
// entry written by an older version still loads instead of blocking startup.
std::vector<std::string> ensure_deployment_label(std::vector<std::string>& labels,
                                                 const std::string& recipe);

} // namespace backends
} // namespace lemon
