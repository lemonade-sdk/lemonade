#pragma once

#include <map>
#include <string>

namespace lemon {
namespace models {

// What one recipe can do on this machine right now.
struct RecipeAvailability {
    // Empty when the recipe's backend can run here.
    std::string unsupported_reason;
    // Empty when a guest backend is installed or none is required.
    std::string guest_backend_missing_reason;
    // True when the backend reads the model from disk on demand, so only the
    // resident working set has to fit in a memory pool.
    bool streams_model_from_storage = false;
};

// Machine facts and per-recipe availability for one rebuild.
//
// The models layer reads nothing but this: system info, backend descriptors and
// runtime configuration are resolved by the probe that builds it, so filtering
// stays a pure function of its inputs.
struct BackendAvailability {
    // An operator can turn visibility filtering off entirely.
    bool filtering_disabled = false;
    // Treat a dGPU as having its GTT aperture available as one pool.
    bool assume_dgpu_gtt = false;

    double system_ram_gb = 0.0;
    double largest_mem_pool_gb = 0.0;

    // Named in the filter reason text, not used to decide anything.
    std::string processor = "Unknown";
    std::string os_version = "Unknown";

    std::map<std::string, RecipeAvailability> recipes;

    // An unknown recipe is reported as supported, matching a probe that has not
    // heard of it.
    const RecipeAvailability& recipe(const std::string& name) const {
        static const RecipeAvailability unknown;
        auto it = recipes.find(name);
        return it != recipes.end() ? it->second : unknown;
    }
};

}  // namespace models
}  // namespace lemon
