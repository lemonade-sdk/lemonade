#include "lemon/models/ingest_collections.h"

#include "lemon/canonical_id.h"
#include "lemon/models/artifact_store.h"
#include "lemon/models/store.h"
#include "lemon/models/json_catalog.h"
#include "lemon/models/registry_documents.h"
#include "lemon/models/registry_types.h"
#include "lemon/model_types.h"
#include "lemon/models/routing_parse_options.h"
#include "lemon/routing_policy_parser.h"
#include "lemon/runtime_config.h"
#include "lemon/utils/aixlog.hpp"
#include "lemon/utils/json_utils.h"
#include "lemon/utils/path_utils.h"
#include "lemon/utils/safe_fs.h"

#include <stdexcept>

#include <filesystem>

namespace fs = std::filesystem;
using nlohmann::json;
using lemon::utils::JsonUtils;
using lemon::utils::path_from_utf8;
using lemon::utils::path_to_utf8;
using lemon::utils::safe_dir_options;

namespace lemon {
namespace models {

namespace {

// Parses an entry the way build_cache does, so two entries can be compared
// field-for-field without the two parsers drifting.
static ModelInfo model_info_from_def(const json& def) {
    ModelInfo info;
    models::parse_checkpoints(info, def);
    info.recipe = JsonUtils::get_or_default<std::string>(def, "recipe", "");
    models::parse_model_source_fields(info, def);
    return info;
}

static std::string collection_component_drift(const ModelInfo& local, const json& manifest_comp) {
    ModelInfo m = model_info_from_def(manifest_comp);
    std::vector<std::string> diffs;
    if (!m.recipe.empty() && m.recipe != local.recipe) {
        diffs.push_back("recipe (manifest='" + m.recipe + "' local='" + local.recipe + "')");
    }
    if (models::registry_source_of(m) != models::registry_source_of(local)) {
        diffs.push_back("source (manifest='" + models::registry_source_of(m) +
                        "' local='" + models::registry_source_of(local) + "')");
    }
    for (const auto& [type, ck] : m.checkpoints) {
        if (ck.empty()) continue;
        auto it = local.checkpoints.find(type);
        std::string lck = (it == local.checkpoints.end()) ? "" : it->second;
        if (ck != lck) {
            diffs.push_back("checkpoint[" + type + "] (manifest='" + ck + "' local='" + lck + "')");
        }
    }
    std::string out;
    for (size_t i = 0; i < diffs.size(); ++i) {
        if (i) out += "; ";
        out += diffs[i];
    }
    return out;
}

// Locate a collection manifest in a cached registry snapshot: any *.json file whose
// content is an object with recipe == "collection.omni". Note the two halves use
// different discovery rules by design: the *initial* remote fetch is
// filename-keyed on <RepoName>.json (see fetch_pull_variants in hf_variants.cpp),
// but once the snapshot is cached this reader is content-based, so the filename is
// not load-bearing here. Returns an empty json when no manifest is cached.
static json read_cached_collection_manifest(const models::ModelArtifactStore& artifacts,
                                           const fs::path& cache_dir) {
    fs::path snap = artifacts.active_snapshot_path(cache_dir);
    if (snap.empty()) return json();

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(snap, safe_dir_options, ec)) {
        std::error_code file_ec;
        if (!entry.is_regular_file(file_ec)) continue;
        if (entry.path().extension() != ".json") continue;
        json candidate;
        try {
            candidate = JsonUtils::load_from_file(path_to_utf8(entry.path()));
        } catch (const std::exception&) {
            continue;
        }
        if (candidate.is_object() &&
            is_model_collection_recipe(JsonUtils::get_or_default<std::string>(candidate, "recipe", ""))) {
            return candidate;
        }
    }
    return json();
}

ModelInfo require_row(const ModelIngest::Deps& deps, const std::string& name) {
    return *deps.store->info(name);
}

}  // namespace

IngestCollections::IngestCollections(const ModelIngest::Deps& deps, ModelIngest& fetch)
    : deps_(deps), fetch_(fetch) {}

json IngestCollections::fetch_manifest(const std::string& repo_id,
                                             const std::string& registry_source,
                                             bool do_not_upgrade) {
    const std::string source = remote_registry_source_name(parse_remote_registry_source(registry_source));
    fs::path cache_dir = path_from_utf8(deps_.artifacts->root()) / repo_id_to_cache_dir_name(repo_id, source);

    // A usable manifest needs both arrays; an incomplete cached copy (e.g. a
    // stale old-format file) must not satisfy do_not_upgrade - it should
    // trigger a refresh instead.
    auto manifest_complete = [](const json& m) {
        return m.is_object() &&
               m.contains("components") && m["components"].is_array() &&
               !m["components"].empty() &&
               m.contains("models") && m["models"].is_array();
    };

    json manifest = read_cached_collection_manifest(*deps_.artifacts, cache_dir);
    bool have_cache = manifest_complete(manifest);

    bool offline = false;
    if (auto* cfg = RuntimeConfig::global()) {
        offline = cfg->offline();
    }

    if (offline) {
        if (!have_cache) {
            throw std::runtime_error(
                "Offline mode: collection definition for '" + repo_id +
                "' is not cached locally.");
        }
    } else if (!(do_not_upgrade && have_cache)) {
        // Download the collection repo (the manifest plus its other small files)
        // into the shared registry cache. A bare repo id with no variant downloads all files.
        ModelInfo manifest_info;
        manifest_info.model_name = repo_id;
        manifest_info.checkpoints["main"] = repo_id;
        try {
            manifest_info.registry_source = source;
            fetch_.fetch_registry_artifacts(manifest_info, nullptr, {});
            manifest = read_cached_collection_manifest(*deps_.artifacts, cache_dir);
        } catch (const std::exception& e) {
            if (!have_cache) throw;
            LOG(WARNING, "ModelManager") << "Could not refresh collection manifest for "
                << repo_id << " (" << e.what() << "); using cached copy" << std::endl;
        }
    }

    if (!manifest_complete(manifest)) {
        throw std::runtime_error(
            "Collection manifest for '" + repo_id +
            "' must contain a non-empty 'components' array and a 'models' array.");
    }
    return manifest;
}

std::vector<std::string> IngestCollections::register_components(
    const json& component_names, const json& component_defs, const std::string& registry_source) {
    auto def_name = [](const json& def) -> std::string {
        if (!def.is_object()) return "";
        for (const char* key : {"model_name", "id"}) {
            if (def.contains(key) && def[key].is_string()) {
                return bare_component_name(def[key].get<std::string>());
            }
        }
        return "";
    };

    // Two passes so register_components is atomic: validate every unknown
    // component's inline definition first, and only register them once the whole
    // list is known good. A half-defined component therefore never persists a
    // stray user.* entry that a later failure would have to roll back.
    std::vector<std::string> components;
    std::vector<std::pair<std::string, json>> pending_registrations;  // canonical, def
    for (const auto& entry : component_names) {
        if (!entry.is_string()) continue;
        std::string name = bare_component_name(entry.get<std::string>());
        if (name.empty()) {
            LOG(WARNING, "ModelManager") << "Skipping collection component with an "
                << "empty name" << std::endl;
            continue;
        }

        // Find the inline definition matching this component, if any.
        json def;
        if (component_defs.is_array()) {
            for (const auto& candidate : component_defs) {
                if (def_name(candidate) == name) {
                    def = candidate;
                    break;
                }
            }
        }

        // A remote collection defines one provenance domain. Components without
        // an explicit source inherit it before drift comparison or registration.
        if (def.is_object() && !def.contains("source") && !def.contains("registry_source")) {
            def["source"] = remote_registry_source_name(
                parse_remote_registry_source(registry_source));
        }

        // Components must be regular models, not collections (backstop for the
        // remote-manifest path, which does not go through validate_collection_request).
        bool def_is_collection =
            def.is_object() && is_model_collection_recipe(def.value("recipe", std::string()));
        if (def_is_collection ||
            (deps_.store->exists(name) && is_model_collection_recipe(require_row(deps_, name).recipe))) {
            throw std::runtime_error(
                "Collection components must be regular models, not collections: '" + name + "'");
        }

        if (deps_.store->exists(name)) {
            // Local-wins: the shipped/registered definition is authoritative. Warn
            // (do not overwrite) when the inline definition has drifted.
            if (def.is_object()) {
                ModelInfo local = require_row(deps_, name);
                std::string drift = collection_component_drift(local, def);
                if (!drift.empty()) {
                    LOG(WARNING, "ModelManager") << "Collection component '" << name
                        << "' differs from the local model definition; using the local "
                        << "definition. Drift: " << drift << std::endl;
                }
            }
            components.push_back(deps_.store->resolve_name(name));
        } else if (def.is_object()) {
            // Unknown: add it from what the file carried, so the file is self-contained.
            std::string canonical = "user." + name;
            // Manifest content is remote input - apply the same reserved-name
            // check as the CLI and /pull registration paths.
            if (is_reserved_registration_name(canonical)) {
                LOG(WARNING, "ModelManager") << "Skipping collection component with "
                    << "reserved name: " << name << std::endl;
                continue;
            }
            // Nothing has been written yet, so refusing here leaves no stray entry.
            if (!collection_component_def_is_valid(def)) {
                throw std::runtime_error(
                    "Collection component '" + name + "' has an incomplete inline "
                    "definition (a recipe and at least one checkpoint are required).");
            }
            pending_registrations.emplace_back(canonical, def);
            components.push_back(canonical);
        } else {
            LOG(WARNING, "ModelManager") << "Skipping unknown collection component with no "
                << "inline definition: " << name << std::endl;
        }
    }

    for (const auto& [canonical, def] : pending_registrations) {
        LOG(INFO, "ModelManager") << "Registering collection component as user model: "
            << canonical << std::endl;
        fetch_.register_user_model(canonical, def);
    }
    return components;
}

std::vector<std::string> IngestCollections::resolve_from_manifest(
    const std::string& repo_id, const std::string& registry_source, bool do_not_upgrade) {
    json manifest = fetch_manifest(repo_id, registry_source, do_not_upgrade);
    return register_components(manifest["components"], manifest["models"], registry_source);
}

std::optional<std::string> IngestCollections::validate(
    const std::string& model_name, const json& model_data) {
    std::string checkpoint_pointer = model_data.value("checkpoint", std::string());
    if (checkpoint_pointer.empty() && model_data.contains("checkpoints") &&
        model_data["checkpoints"].is_object()) {
        checkpoint_pointer = model_data["checkpoints"].value("main", std::string());
    }
    const bool has_components =
        model_data.contains("components") && model_data["components"].is_array() &&
        !model_data["components"].empty();
    if (!has_components) {
        if (!checkpoint_pointer.empty()) {
            return std::nullopt;  // pointer-only registry-backed collection
        }
        return std::string("Collection recipe requires a non-empty 'components' array");
    }
    // An inline collection import carries its component definitions in a `models`
    // array. Every component must be *resolvable*: either it is already
    // registered locally (local-wins) or it has a matching definition in
    // `models`. Validate this per-component and fail closed. `models` being
    // present is not a blanket license - a component it omits would otherwise
    // pass here and then be silently skipped during registration, persisting a
    // collection smaller than the imported file describes. Mirror the
    // name-matching that register_components() uses (bare names; `model_name`
    // or `id` keys) so this gate rejects exactly what registration would drop.
    const bool has_models_array =
        model_data.contains("models") && model_data["models"].is_array();
    auto inline_def_name = [](const json& def) -> std::string {
        if (!def.is_object()) return "";
        for (const char* key : {"model_name", "id"}) {
            if (def.contains(key) && def[key].is_string()) {
                return bare_component_name(def[key].get<std::string>());
            }
        }
        return "";
    };
    auto find_inline_def = [&](const std::string& bare) -> const json* {
        if (!has_models_array) return nullptr;
        for (const auto& candidate : model_data["models"]) {
            if (inline_def_name(candidate) == bare) return &candidate;
        }
        return nullptr;
    };
    const std::string bare_collection = bare_component_name(model_name);
    for (const auto& component : model_data["components"]) {
        if (!component.is_string()) {
            return std::string("components entries must be strings");
        }
        std::string component_name = component.get<std::string>();
        std::string bare = bare_component_name(component_name);
        // Self-reference: compare bare forms so a collection `user.MyCol` with
        // `components: ["MyCol"]` is rejected too, not only the exact-string case.
        if (bare == bare_collection) {
            return "Collection cannot reference itself: " + component_name;
        }
        // Components must be regular models, not collections.
        const json* def = find_inline_def(bare);
        bool is_collection_component =
            (def != nullptr && def->is_object() &&
             is_model_collection_recipe(def->value("recipe", std::string()))) ||
            (deps_.store->exists(bare) && is_model_collection_recipe(require_row(deps_, bare).recipe));
        if (is_collection_component) {
            return "Collection components must be regular models, not collections: '" +
                   component_name + "'";
        }
        if (has_models_array) {
            if (!deps_.store->exists(bare) && def == nullptr) {
                return "Inline collection component '" + component_name +
                       "' has no matching definition in 'models' and is not a "
                       "registered model. Every component must be defined inline "
                       "(matching 'model_name') or reference an already-registered "
                       "model.";
            }
            // A component that resolves to its inline definition (rather than an
            // already-registered model) must be a usable registration. Reject a
            // half-defined component here instead of registering it and failing
            // later mid-download.
            if (!deps_.store->exists(bare) && def != nullptr &&
                !collection_component_def_is_valid(*def)) {
                return "Inline collection component '" + component_name +
                       "' has an incomplete definition in 'models' (a recipe and "
                       "at least one checkpoint are required).";
            }
            // register_user_model() would throw on this once the import reached
            // registration, leaving a partly-imported collection behind. Report
            // it here, where the import can still be refused whole.
            if (!deps_.store->exists(bare) && def != nullptr) {
                std::string illegal;
                normalized_definition_labels(*def, &illegal);
                if (!illegal.empty()) {
                    return describe_illegal_labels(component_name, illegal);
                }
            }
        } else if (!deps_.store->exists(component_name)) {
            return "Collection component not registered: '" + component_name +
                   "'. Pull or register it before referencing it in a collection.";
        }
    }
    if (is_router_collection_recipe(model_data.value("recipe", std::string()))) {
        // Same resolution shape as build_cache()'s, differing only in its
        // tiers: this one resolves live and falls back to the request's own
        // inline `models[]`, because an import validates names that are not
        // registered yet. Unresolvable names are kept as authored by
        // make_routing_parse_options, so component-role validation can still
        // compare them against the authored components array.
        RoutingPolicyParseOptions options = make_routing_parse_options({
            [this](const std::string& name) -> std::optional<std::string> {
                try {
                    return deps_.store->resolve_name(name);
                } catch (...) {
                    return std::nullopt;
                }
            },
            [this](const std::string& name) -> std::optional<ModelType> {
                try {
                    return require_row(deps_, name).type;
                } catch (...) {
                    return std::nullopt;
                }
            },
            [this, &find_inline_def](const std::string& name) -> std::optional<ModelType> {
                // Not registered - the name may match an inline `models[]`
                // definition in this same request. Derive the type exactly as
                // register_user_model() would once that definition is
                // registered, so validation and runtime cannot disagree
                // (absent labels default to LLM, same as everywhere else).
                // nullopt only when no definition exists at all - the
                // "truly unknown" case.
                const json* def = find_inline_def(bare_component_name(name));
                if (!def) {
                    return std::nullopt;
                }
                std::set<std::string> label_set = normalized_definition_labels(*def);
                return get_model_type_from_labels(
                    std::vector<std::string>(label_set.begin(), label_set.end()));
            },
        });
        try {
            // Strip pull-protocol keys (stream, subscribe, do_not_upgrade, …)
            // that the client merges into the body but the policy parser does
            // not know about. Passing the raw request body would trigger the
            // reject_unknown_keys check inside parse_route_policy_collection.
            json policy_json = json::object();
            for (const auto& key : routing_policy_root_keys()) {
                if (model_data.contains(key)) policy_json[key] = model_data.at(key);
            }
            RoutePolicy policy = parse_route_policy_collection(policy_json, options);
            RoutingPolicyEngine(std::move(policy), ClassifierServices{});
        } catch (const std::exception& e) {
            return std::string("Invalid collection.router routing policy: ") + e.what();
        }
    }
    return std::nullopt;
}

void IngestCollections::populate_from_cached_manifest(ModelInfo& info) {
    std::string repo_id = info.checkpoint();
    if (repo_id.empty()) return;

    fs::path cache_dir = path_from_utf8(deps_.artifacts->root()) /
        repo_id_to_cache_dir_name(repo_id, models::registry_source_of(info));
    json manifest = read_cached_collection_manifest(*deps_.artifacts, cache_dir);
    if (!manifest.is_object() || !manifest.contains("components") ||
        !manifest["components"].is_array()) {
        return;
    }

    const models::RegistryDocuments::Pin registries = deps_.registries->pin();

    for (const auto& entry : manifest["components"]) {
        if (!entry.is_string()) continue;
        std::string name = bare_component_name(entry.get<std::string>());
        std::string canonical;
        if (registries.contains(models::RegistryDocument::Builtin, name)) {
            canonical = name;
        } else if (registries.contains(models::RegistryDocument::User, name)) {
            canonical = "user." + name;
        } else {
            canonical = name;  // not yet registered → resolves as not-downloaded
        }
        info.components.push_back(canonical);
    }

    // Surface the collection's total download size when the slim registry entry
    // omits it.
    if (info.size == 0.0 && manifest.contains("size") && manifest["size"].is_number()) {
        info.size = manifest["size"].get<double>();
    }

    // Same lift-from-manifest pattern for the optional per-collection system
    // prompt: the registry entry wins when it sets one, otherwise the published
    // remote manifest acts as the source of truth.
    if (info.system_prompt.empty() && manifest.contains("system_prompt") &&
        manifest["system_prompt"].is_string()) {
        info.system_prompt = manifest["system_prompt"].get<std::string>();
    }
}

}  // namespace models
}  // namespace lemon
