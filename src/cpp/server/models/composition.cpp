#include <lemon/models/composition.h>

#include <lemon/backends/backend_descriptor_registry.h>
#include <lemon/backends/backend_registry.h>
#include <lemon/canonical_id.h>
#include <lemon/models/model_info.h>
#include <lemon/model_types.h>
#include <lemon/models/artifact_store.h>
#include <lemon/models/catalog.h>
#include <lemon/models/entry_identity.h>
#include <lemon/models/ingest.h>
#include <lemon/models/recipe_option_layers.h>
#include <lemon/models/routing_parse_options.h>
#include <lemon/routing_policy.h>
#include <lemon/routing_policy_parser.h>
#include <lemon/utils/aixlog.hpp>

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace lemon {
namespace models {


// Check if all components of a collection model are downloaded.
static bool check_component_downloaded(const ModelInfo& info,
                                        const std::map<std::string, ModelInfo>& model_map) {
    if (info.components.empty()) return false;
    for (const auto& component_name : info.components) {
        auto it = model_map.find(component_name);
        if (it == model_map.end() || !it->second.downloaded) {
            return false;
        }
    }
    return true;
}

static std::map<std::string, ModelInfo> host_visible_probe(const std::map<std::string, ModelInfo>& m) { return m; }
static nlohmann::json empty_policy_doc() { return nlohmann::json{{"recipe", "router"}, {"components", nlohmann::json::array()}}; }

CompositionResult ModelComposition::run() {
    std::map<std::string, ModelInfo> composed;
    std::shared_ptr<const FilterReasons> composed_reasons;

    // Collected as rows parse so the options layering never re-reads a document.
    std::map<std::string, json> json_recipe_options;

    CatalogSet built;
    deps_.load_catalogs(json_recipe_options, built);

    CompositionResult result;

    CatalogAccumulator accumulated;
    for (const auto& catalog : built.snapshot()) {
        catalog->enumerate(accumulated);
    }

    std::map<std::string, ModelInfo> all_models = accumulated.take();

    // Clients are not guaranteed to handle a model that declares no deployment
    // label. Every ingest path above stamps one; name any model that reached
    // here without.
    for (const auto& [name, info] : all_models) {
        ModelType mode = ModelType::LLM;
        if (!find_deployment_mode(info.labels, mode)) {
            LOG(WARNING, "ModelComposition")
                << "Model '" << name << "' (recipe " << info.recipe
                << ") has no deployment label; add one (chat, transcription, embeddings, ...)"
                << std::endl;
        }
    }

    const json saved_options = deps_.saved_options();
    for (auto& [name, info] : all_models) {
        json jro = json_recipe_options.count(name) ? json_recipe_options[name] : json(nullptr);
        info.recipe_options =
            build_recipe_options(info, jro, cache_key_to_canonical_id(name), saved_options);
        if (info.recipe_options.to_json().contains("auto_update") &&
            info.recipe_options.to_json()["auto_update"].is_boolean()) {
            info.auto_update = info.recipe_options.to_json()["auto_update"].get<bool>();
        }
    }

    // Pre-filter snapshots for the routing-policy resolver below (#2748); see
    // compute_model_alias_maps() for why the alias needs the full algorithm
    // rather than a plain bare-name map. Only router collections read them and
    // the alias rebuild is O(n log n) over the whole registry, so a host with
    // no router collection -- the common case, across ~15 build_cache() call
    // sites -- pays nothing.
    const bool needs_routing_snapshots =
        std::any_of(all_models.begin(), all_models.end(), [](const auto& entry) {
            return is_router_collection_recipe(entry.second.recipe);
        });

    std::map<std::string, ModelType> pre_filter_types;
    std::map<std::string, std::string> pre_filter_aliases;
    if (needs_routing_snapshots) {
        for (const auto& [name, info] : all_models) {
            pre_filter_types[name] = info.type;
        }
        pre_filter_aliases = compute_model_alias_maps(all_models).public_model_aliases;
    }

    // Step 2: Filter by backend availability. This is the full-registry pass, so
    // its reasons replace the recipe availability table wholesale.
    HostFilter host = filter_for_host(all_models, deps_.availability);
    composed_reasons = host.reasons;
    all_models = std::move(host.visible);

    // Step 3: Check download status for all models. Dynamic-discovery backends
    // (flm, cloud) already set downloaded during discovery; everyone else asks
    // its backend ops (default = shared registry-cache completeness check).
    backends::BackendOpsContext status_ctx;
    status_ctx.fetch = deps_.fetch;
    status_ctx.artifacts = deps_.artifacts;

    int downloaded_count = 0;
    // First pass: determine download status for non-collection models
    for (auto& [name, info] : all_models) {
        if (is_model_collection_recipe(info.recipe)) {
            continue;  // Handled in second pass after components are resolved
        }
        const auto* desc = backends::descriptor_for(info.recipe);
        if (!(desc && desc->dynamic_models)) {
            info.downloaded = backends::ops_for(info.recipe)->is_downloaded(info, status_ctx);
        }

        if (info.downloaded) {
            downloaded_count++;
        }
    }

    // Second pass: determine download status for collection models
    // (must happen after components have their downloaded status set)
    for (auto& [name, info] : all_models) {
        if (!is_model_collection_recipe(info.recipe)) continue;
        info.downloaded = check_component_downloaded(info, all_models);
        if (info.downloaded) {
            downloaded_count++;
        }
    }

    for (auto& [name, info] : all_models) {
        populate_model_metadata(info, *deps_.artifacts);
        if (info.downloaded && deps_.artifacts->download_policy(info) !=
            backends::DownloadPolicy::SelfManaged) {
            ModelArtifactStore::refresh_on_disk_size(info);
        }
        composed[name] = std::move(info);
    }

    // A policy resolves against the candidate, not the store: the set this build
    // publishes is not published yet. Its aliases come from before the host filter
    // on purpose, so a component that exists but cannot run here is reported as
    // unavailable — a classifier bound to it takes its on_error path on every
    // request — rather than as a typo (#2748).
    std::set<std::string> unavailable_components;

    RoutingPolicyParseOptions policy_options = make_routing_parse_options({
        [&pre_filter_aliases](const std::string& name) -> std::optional<std::string> {
            auto it = pre_filter_aliases.find(name);
            if (it == pre_filter_aliases.end()) return std::nullopt;
            return it->second;
        },
        [&composed](const std::string& name) -> std::optional<ModelType> {
            auto it = composed.find(name);
            if (it == composed.end()) return std::nullopt;
            return it->second.type;
        },
        [&pre_filter_types, &unavailable_components](
            const std::string& name) -> std::optional<ModelType> {
            auto it = pre_filter_types.find(name);
            if (it == pre_filter_types.end()) return std::nullopt;
            unavailable_components.insert(name);
            return it->second;
        },
    });
    for (auto& [name, info] : composed) {
        if (!is_router_collection_recipe(info.recipe)) {
            continue;
        }
        json doc;
        doc["recipe"] = info.recipe;
        doc["components"] = info.components;
        auto version_it = info.extras.find("version");
        if (version_it != info.extras.end()) {
            doc["version"] = version_it->second;
        }
        auto routing_it = info.extras.find("routing");
        if (routing_it != info.extras.end()) {
            doc["routing"] = routing_it->second;
        }
        unavailable_components.clear();
        try {
            info.route_policy = std::make_shared<const RoutePolicy>(
                parse_route_policy_collection(doc, policy_options));
        } catch (const std::exception& e) {
            LOG(WARNING, "ModelComposition") << "Failed to parse routing policy for '"
                                         << name << "': " << e.what() << std::endl;
        }
        if (!unavailable_components.empty()) {
            std::ostringstream joined;
            bool first = true;
            for (const auto& component : unavailable_components) {
                if (!first) joined << ", ";
                joined << component;
                first = false;
            }
            LOG(WARNING, "ModelComposition")
                << "Routing policy for '" << name << "' references component(s) "
                << "unavailable on this host: " << joined.str()
                << ". Any classifier bound to them will fall back to its on_error "
                << "result on every request." << std::endl;
        }
    }


    result.models = std::move(composed);
    result.reasons = std::move(composed_reasons);
    result.downloaded = downloaded_count;
    result.catalogs = built.snapshot();
    return result;
}

void populate_model_metadata(ModelInfo& info, const ModelArtifactStore& artifacts) {
    // A route to someone else's GPU has no local bytes to read metadata from.
    if (artifacts.download_policy(info) == backends::DownloadPolicy::None) {
        return;
    }
    info.max_context_window = 0;
    if (!info.downloaded) return;

    // Per-backend metadata (GGUF arch/labels for llamacpp, config.json ctx for
    // flm, ...) is read by the backend's ops, not a recipe switchboard here.
    backends::BackendOpsContext ctx;
    backends::ops_for(info.recipe)->populate_metadata(info, ctx);
}

}  // namespace models
}  // namespace lemon
