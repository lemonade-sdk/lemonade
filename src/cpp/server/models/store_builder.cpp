#include <lemon/canonical_id.h>
#include <lemon/model_registry.h>
#include <lemon/model_types.h>
#include <lemon/models/store_builder.h>
#include <lemon/models/store_snapshot.h>
#include <lemon/routing_policy.h>

#include <algorithm>
#include <sstream>

namespace lemon {
namespace models {

std::set<std::string> collect_helper_models(
    const std::map<std::string, ModelInfo>& models) {
    std::set<std::string> needed;
    for (const auto& [name, info] : models) {
        if (!info.route_policy) {
            continue;
        }
        needed.insert(info.route_policy->helper_models.begin(),
                      info.route_policy->helper_models.end());
    }
    return needed;
}

namespace {

// Registered or discovered locally: never hidden by machine-fit rules.
bool names_a_local_model(const std::string& canonical_id_value) {
    const auto parsed = parse_canonical_id(canonical_id_value);
    return parsed && parsed->source != ModelSource::Builtin;
}

}  // namespace

std::set<std::string> recipes_missing_all_models(
    const std::set<std::string>& size_filtered_recipes,
    const std::set<std::string>& visible_recipes) {
    std::set<std::string> hidden;
    for (const auto& recipe : size_filtered_recipes) {
        if (visible_recipes.find(recipe) == visible_recipes.end()) {
            hidden.insert(recipe);
        }
    }
    return hidden;
}

double streaming_working_set_gb(double min_resident_gb, double size_gb) {
    return min_resident_gb > 0.0 ? min_resident_gb : size_gb;
}

bool streaming_model_exceeds_pool(double working_set_gb, double pool_gb) {
    // pool <= 0 means the device pool is unknown: don't hide on missing data.
    return pool_gb > 0.0 && working_set_gb > pool_gb;
}

FilterResult filter_hardware(const std::map<std::string, ModelInfo>& models,
                             const BackendAvailability& availability) {
    FilterResult result;

    if (availability.filtering_disabled) {
        result.visible = models;
        return result;
    }

    const double max_model_size_gb = availability.largest_mem_pool_gb >
                                             (availability.system_ram_gb * 0.8)
                                         ? availability.largest_mem_pool_gb
                                         : (availability.system_ram_gb * 0.8);

    std::set<std::string> size_filtered_recipes;
    std::set<std::string> visible_recipes;

    for (const auto& [name, info] : models) {
        const std::string& recipe = info.recipe;
        const RecipeAvailability& recipe_availability = availability.recipe(recipe);
        bool filter_out = false;
        std::string filter_reason;
        RejectCause cause = RejectCause::BackendMissing;

        // Collections orchestrate components; visible whenever present.
        if (is_model_collection_recipe(recipe)) {
            result.visible[name] = info;
            continue;
        }

        // Cloud-offloaded models run remotely: no local backend or RAM check.
        if (recipe == "cloud") {
            result.visible[name] = info;
            continue;
        }

        const bool user_controlled_model = names_a_local_model(name) ||
                                           info.source == local_origin::upload;

        if (!recipe_availability.unsupported_reason.empty()) {
            filter_out = true;
            filter_reason = recipe_availability.unsupported_reason + " "
                            "Detected processor: " + availability.processor + ". "
                            "Detected operating system: " + availability.os_version + ".";
        }

        if (!filter_out && !recipe_availability.guest_backend_missing_reason.empty()) {
            filter_out = true;
            filter_reason = recipe_availability.guest_backend_missing_reason;
        }

        // Filter out models too large to run on this machine.
        if (!filter_out && !user_controlled_model && info.size > 0.0) {
            if (recipe_availability.streams_model_from_storage) {
                // A streaming backend reads from disk on demand: only the
                // resident working set must fit, in the device's own pool. ROCm
                // cannot address system RAM and the iGPU carveout as one pool,
                // so its ceiling is the larger pool.
                const double working_set = streaming_working_set_gb(info.min_resident_gb, info.size);
                if (streaming_model_exceeds_pool(working_set, availability.largest_mem_pool_gb)) {
                    filter_out = true;
                    cause = RejectCause::MemoryFit;
                    size_filtered_recipes.insert(recipe);
                    std::ostringstream oss;
                    oss << std::fixed << std::setprecision(1);
                    oss << "This model streams from disk but needs about " << working_set
                        << " GB resident in GPU memory, and this device's largest memory "
                        << "pool is only " << availability.largest_mem_pool_gb << " GB.";
                    filter_reason = oss.str();
                }
            } else if (availability.system_ram_gb > 0.0 && info.size > max_model_size_gb) {
                // Non-streaming: the whole model must fit (80% of system RAM).
                filter_out = true;
                cause = RejectCause::MemoryFit;
                size_filtered_recipes.insert(recipe);
                std::ostringstream oss;
                oss << std::fixed << std::setprecision(1);
                oss << "This model requires approximately " << info.size << " GB of memory, "
                    << "but your system only has " << availability.system_ram_gb << " GB of RAM. "
                    << "Models larger than " << max_model_size_gb
                    << " GB (80% of system RAM) are filtered out.";
                filter_reason = oss.str();
            }
        }

        // Special rule: filter out gpt-oss-20b-FLM on Windows systems with less than 48 GB RAM
#ifdef _WIN32
        if (!filter_out && name == "gpt-oss-20b-FLM" && availability.system_ram_gb > 0.0 &&
            availability.system_ram_gb < 48.0) {
            filter_out = true;
            cause = RejectCause::MemoryFit;
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(1);
            oss << "The gpt-oss-20b-FLM model requires at least 48 GB of RAM. "
                << "Your system has " << availability.system_ram_gb << " GB.";
            filter_reason = oss.str();
        }

        // Special rule: filter out qwen3.6-moe-35b-a3b-FLM on Windows systems with less than 64 GB RAM
        if (!filter_out && name == "qwen3.6-moe-35b-a3b-FLM" &&
            availability.system_ram_gb > 0.0 && availability.system_ram_gb < 64.0) {
            filter_out = true;
            cause = RejectCause::MemoryFit;
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(1);
            oss << "The qwen3.6-moe-35b-a3b-FLM model requires at least 64 GB of RAM. "
                << "Your system has " << availability.system_ram_gb << " GB.";
            filter_reason = oss.str();
        }
#endif

        if (filter_out) {
            result.rejected.push_back(RejectedModel{name, filter_reason, cause});
            continue;
        }

        // Model passes all filters
        visible_recipes.insert(info.recipe);
        result.visible[name] = info;
    }

    for (const auto& recipe : recipes_missing_all_models(size_filtered_recipes, visible_recipes)) {
        result.empty_recipes.push_back(recipe);
    }

    return result;
}

std::vector<std::pair<std::string, bool>> collection_states_after_component(
    const std::map<std::string, ModelInfo>& models, const std::string& model_name,
    bool component_downloaded) {
    std::vector<std::pair<std::string, bool>> changed;
    for (const auto& [name, entry] : models) {
        if (!is_model_collection_recipe(entry.recipe) || entry.components.empty()) continue;
        if (std::find(entry.components.begin(), entry.components.end(), model_name) ==
            entry.components.end()) {
            continue;
        }
        bool all_present = true;
        for (const auto& component_name : entry.components) {
            if (component_name == model_name) {
                if (!component_downloaded) all_present = false;
            } else {
                auto component = models.find(component_name);
                if (component == models.end() || !component->second.downloaded) all_present = false;
            }
            if (!all_present) break;
        }
        if (entry.downloaded != all_present) changed.emplace_back(name, all_present);
    }
    return changed;
}

}  // namespace models
}  // namespace lemon
