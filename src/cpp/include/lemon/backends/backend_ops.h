#pragma once

#include <string>
#include <vector>

namespace lemon {

struct ModelInfo;
class ModelManager;

namespace backends {

// Context handed to BackendOps methods — the bits of server state model
// management needs without a running subprocess. Grows as migrations require.
struct BackendOpsContext {
    ModelManager* model_manager = nullptr;
};

// Stateless per-backend behavior for model management that happens WITHOUT a
// running subprocess: checkpoint-path resolution, download, dynamic discovery,
// per-model metadata, version detection, availability. One singleton per
// backend, exposed via lemon::backends::<stem>::ops() and bound in the registry
// (see BackendRegistration::ops).
//
// The base class is the shared default behavior (the common HF-backed case);
// each backend folder overrides ONLY the policy points it needs, so shared
// logic is inherited rather than copied. Methods are added here incrementally as
// switchboards in model_manager / system_info are migrated; every method has a
// default so adding one never forces edits to backends that don't override it.
class BackendOps {
public:
    virtual ~BackendOps() = default;

    // Populate model-specific metadata (context window, capability labels, …)
    // for a downloaded model. Default: nothing.
    virtual void populate_metadata(ModelInfo& info, const BackendOpsContext& ctx) const {
        (void)info;
        (void)ctx;
    }
};

// Shared default ops instance for backends that override nothing.
const BackendOps* default_backend_ops();

} // namespace backends
} // namespace lemon
