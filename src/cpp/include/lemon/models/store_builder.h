#pragma once

#include <lemon/model_manager.h>
#include <lemon/models/backend_availability.h>

#include <map>
#include <set>
#include <string>
#include <vector>

namespace lemon {
namespace models {

// Why a model is not offered on this machine.
enum class RejectCause {
    BackendMissing,
    MemoryFit,
    RecipeEmpty,
    RegistryUnknown,
};

struct RejectedModel {
    std::string canonical_id;
    // The text a caller returns for an unavailable model.
    std::string reason;
    RejectCause cause;
};

// The visible set plus why everything else is not.
struct FilterResult {
    std::map<std::string, ModelInfo> visible;
    std::vector<RejectedModel> rejected;
    // Recipes whose every model was rejected by memory fit.
    std::vector<std::string> empty_recipes;
};

// The reasons from a FilterResult, without a second copy of the model map.
struct FilterReasons {
    std::vector<RejectedModel> rejected;
    std::vector<std::string> empty_recipes;
};

// Recipes left with nothing visible after memory fit.
std::set<std::string> recipes_missing_all_models(
    const std::set<std::string>& size_filtered_recipes,
    const std::set<std::string>& visible_recipes);

// Declared resident footprint, or the whole model when nothing smaller is known.
double streaming_working_set_gb(double min_resident_gb, double size_gb);

// An unknown pool (<= 0) hides nothing.
bool streaming_model_exceeds_pool(double working_set_gb, double pool_gb);

// What this machine can run now, and why not the rest. Pure.
FilterResult filter_hardware(const std::map<std::string, ModelInfo>& models,
                             const BackendAvailability& availability);

}  // namespace models
}  // namespace lemon
