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
// The backend reads from disk, so only the resident working set must fit.
    bool streams_model_from_storage = false;
};

// Machine facts for one rebuild; filtering is a pure function of these.
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

// An unknown recipe counts as supported.
    const RecipeAvailability& recipe(const std::string& name) const {
        static const RecipeAvailability unknown;
        auto it = recipes.find(name);
        return it != recipes.end() ? it->second : unknown;
    }
};

}  // namespace models
}  // namespace lemon
