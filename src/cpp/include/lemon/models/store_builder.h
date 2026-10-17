#pragma once

#include <lemon/model_manager.h>
#include <lemon/models/backend_availability.h>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>
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

// One hardware-filter pass, ready for whoever ran it to keep: the visible set,
// and the reasons the rest were held back.
struct HostFilter {
    std::map<std::string, ModelInfo> visible;
    std::shared_ptr<const FilterReasons> reasons;
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

// Collections that list `model_name` among their components, each with the
// download state it has once that component has `component_downloaded`. Only the
// collections whose state moves come back: a caller applies the result as a
// patch. A component row missing from `models` counts as not downloaded.
std::vector<std::pair<std::string, bool>> collection_states_after_component(
    const std::map<std::string, ModelInfo>& models, const std::string& model_name,
    bool component_downloaded);

}  // namespace models
}  // namespace lemon
