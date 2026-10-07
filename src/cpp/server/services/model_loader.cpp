#include "lemon/server/model_loader.h"

#include <algorithm>
#include <vector>

#include <lemon/utils/aixlog.hpp>

#include "lemon/model_registry.h"
#include "lemon/system_info.h"

namespace lemon {

using json = nlohmann::json;

ModelLoader::ModelLoader(Router* router, ModelManager* model_manager)
    : router_(router), model_manager_(model_manager) {}

json ModelLoader::load_options(const json& request) {
    json result = json::object();
    if (request.is_object() && request.contains("ctx_size")) {
        result["ctx_size"] = request["ctx_size"];
    }
    return result;
}

void ModelLoader::ensure_loaded(const std::string& requested_model, const json& load_options,
                                LoadPurpose load_purpose) {
    // A live process follows its current use without a reload: routing work
    // promotes it to RoutingHelper, while direct inference demotes it into the
    // counted Standard pool. Destination-pool admission remains authoritative.
    if (router_->ensure_loaded_model_residency(requested_model, load_purpose)) {
        LOG(DEBUG, "Server")
            << "Model already loaded: " << requested_model
            << " (residency="
            << residency_class_to_string(residency_class_for_load_purpose(load_purpose))
            << ")" << std::endl;
        if (load_options.contains("ctx_size")) {
            auto loaded_ctx = router_->get_model_recipe_options(requested_model)
                                  .get_option("ctx_size");
            LOG(DEBUG, "Server")
                << "Ignoring requested ctx_size=" << load_options["ctx_size"]
                << " for already-loaded " << requested_model
                << " (loaded ctx_size=" << loaded_ctx << ")" << std::endl;
        }
        return;
    }

    LOG(INFO, "Server") << "Auto-loading model: " << requested_model << std::endl;

    if (!model_manager_->model_exists(requested_model)) {
        throw std::runtime_error("Model not found: " + requested_model);
    }

    auto info = model_manager_->get_model_info(requested_model);

    if (is_omni_collection_recipe(info.recipe)) {
        ensure_collection_loaded(info);
        return;
    }

    // Never check the registry for updates here (do_not_upgrade=true): a model that is
    // not downloaded yet is fetched from its recorded registry, and a downloaded one is
    // used as cached. Only /pull checks for updates.
    if (!model_manager_->backend_self_manages_downloads(info.recipe) &&
        !model_manager_->is_model_downloaded(requested_model)) {
        LOG(INFO, "Server") << "Model not cached, downloading from "
                            << remote_registry_display_name(
                                   parse_remote_registry_source(info.registry_source))
                            << "..." << std::endl;
        LOG(INFO, "Server") << "This may take several minutes for large models." << std::endl;
        model_manager_->download_registered_model(info, true);
        LOG(INFO, "Server") << "Model download complete: " << requested_model << std::endl;

        // resolved_path is computed from the filesystem, so refresh it now that the
        // files exist.
        info = model_manager_->get_model_info(requested_model);
    }

    router_->load_model(requested_model, info,
                        RecipeOptions(info.recipe, load_options), true,
                        /*allow_reload_on_option_change=*/false,
                        /*pinned=*/std::nullopt,
                        load_purpose);
    LOG(INFO, "Server") << "Model loaded successfully: " << requested_model << std::endl;
}

void ModelLoader::ensure_collection_loaded(const ModelInfo& info) {
    LOG(INFO, "Server") << "Loading collection components for: " << info.model_name << std::endl;
    for (const auto& component : info.components) {
        if (!model_manager_->model_exists(component)) {
            LOG(WARNING, "Server") << "Skipping unknown component: " << component << std::endl;
            continue;
        }
        if (router_->is_model_loaded(component)) {
            LOG(DEBUG, "Server") << "Component already loaded: " << component << std::endl;
            continue;
        }
        auto comp_info = model_manager_->get_model_info(component);
        if (!comp_info.downloaded) {
            LOG(INFO, "Server") << "Downloading component: " << component << std::endl;
            model_manager_->download_registered_model(comp_info);
            comp_info = model_manager_->get_model_info(component);
        }
        LOG(INFO, "Server") << "Loading component: " << component << std::endl;
        // The collection load request does not become the component request
        // layer. Router already reads the component's saved options from comp_info,
        // so an empty request preserves normal model/architecture/backend scope
        // semantics.
        router_->load_model(
            component, comp_info, RecipeOptions(comp_info.recipe, json::object()), true,
            /*allow_reload_on_option_change=*/true);
    }
}

json ModelLoader::model_error(const std::string& requested_model,
                              const std::string& exception_msg) const {
    json error_response;

    // Case 1: the model exists but is filtered out on this system (e.g. an NPU model
    // without an NPU).
    std::string filter_reason = model_manager_->get_model_filter_reason(requested_model);
    if (!filter_reason.empty()) {
        std::string message = "Model '" + requested_model + "' is not available on this system. " + filter_reason;
        error_response["error"] = {
            {"message", message},
            {"type", "model_not_supported"},
            {"param", "model"},
            {"code", "model_not_supported"},
            {"requested_model", requested_model}
        };
        return error_response;
    }

    // Case 2: the model is not in the registry at all.
    if (!model_manager_->model_exists(requested_model)) {
        std::string message = "Model '" + requested_model + "' was not found. ";

        auto available_models = model_manager_->get_supported_models();
        if (!available_models.empty()) {
            std::vector<std::string> model_names;
            model_names.reserve(available_models.size());
            for (const auto& [name, info] : available_models) {
                model_names.push_back(name);
            }
            std::sort(model_names.begin(), model_names.end());

            const size_t max_suggestions = 3;
            size_t count = std::min(model_names.size(), max_suggestions);

            message += "Available models include: ";
            for (size_t i = 0; i < count; ++i) {
                if (i > 0) message += ", ";
                message += "'" + model_names[i] + "'";
            }

            if (model_names.size() > max_suggestions) {
                message += ", and " + std::to_string(model_names.size() - max_suggestions) + " more";
            }
            message += ". ";
        }

        message += "Use 'lemonade list' or GET /api/v1/models?show_all=true to see all available models.";

        // A -FLM name usually means the FLM backend is not ready yet.
        if (requested_model.size() > 4 &&
            requested_model.substr(requested_model.size() - 4) == "-FLM") {
            auto flm_status = SystemInfoCache::get_flm_status();
            if (!flm_status.is_ready()) {
                message += " The FLM backend is not ready: " + flm_status.message + ".";
                if (!flm_status.action.empty()) {
                    message += " " + flm_status.action + ".";
                }
            }
        }

        error_response["error"] = {
            {"message", message},
            {"type", "model_not_found"},
            {"param", "model"},
            {"code", "model_not_found"},
            {"requested_model", requested_model}
        };
        return error_response;
    }

    // Case 3: the model exists and is available, but failed to load.
    if (exception_msg.rfind("Routing residency conflict:", 0) == 0) {
        error_response["error"] = {
            {"message", exception_msg},
            {"type", "router_residency_conflict"},
            {"param", "model"},
            {"code", "router_residency_conflict"},
            {"requested_model", requested_model}
        };
        return error_response;
    }

    if (exception_msg.find("are pinned") != std::string::npos) {
        error_response["error"] = {
            {"message", exception_msg},
            {"type", "slots_pinned_error"},
            {"param", "model"},
            {"code", "slots_pinned_error"},
            {"requested_model", requested_model}
        };
        return error_response;
    }

    std::string message = "Failed to load model '" + requested_model + "': " + exception_msg;
    error_response["error"] = {
        {"message", message},
        {"type", "model_load_error"},
        {"param", "model"},
        {"code", "model_load_error"},
        {"requested_model", requested_model}
    };
    return error_response;
}

int ModelLoader::load_error_status(const std::string& error_code) {
    if (error_code == "slots_pinned_error" ||
        error_code == "router_residency_conflict") {
        return 409;
    } else if (error_code == "model_load_error") {
        return 500;
    }
    return 404;
}

void ModelLoader::write_load_error(httplib::Response& res, const std::string& model,
                                   const std::string& message) const {
    json error_response = model_error(model, message);
    res.status = load_error_status(error_response["error"]["code"].get<std::string>());
    res.set_content(error_response.dump(), "application/json");
}

} // namespace lemon
