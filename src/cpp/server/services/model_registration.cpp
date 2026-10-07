#include "lemon/server/model_registration.h"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <stdexcept>

#include <lemon/utils/aixlog.hpp>

#include "lemon/backends/backend_registry.h"
#include "lemon/model_manager.h"
#include "lemon/model_registry.h"
#include "lemon/utils/path_utils.h"

namespace lemon {

using json = nlohmann::json;

namespace {

void validate_registration_name(const std::string& model_name, bool require_user_namespace) {
    if (lemon::is_reserved_registration_name(model_name)) {
        throw std::invalid_argument(
            "Model names with 'extra.' / 'builtin.' prefixes are reserved, "
            "including as bare-name parts of a 'user.' alias. Received: " + model_name);
    }

    if (require_user_namespace &&
        (model_name.size() <= 5 || model_name.rfind("user.", 0) != 0)) {
        throw std::invalid_argument(
            "Registered model definitions must use a non-empty `user.*` name, "
            "for example `user.Phi-4-Mini-GGUF`. Received: " + model_name);
    }
}

void normalize_registration_source(json& request_json, bool local_import) {
    if (!request_json.contains("source") && !request_json.contains("registry_source")) {
        return;
    }

    std::optional<std::string> normalized_registry;
    if (request_json.contains("registry_source")) {
        if (!request_json["registry_source"].is_string()) {
            throw std::invalid_argument("`registry_source` must be a string when provided");
        }
        normalized_registry = remote_registry_source_name(
            parse_remote_registry_source(
                request_json["registry_source"].get<std::string>()));
    }

    if (request_json.contains("source")) {
        if (!request_json["source"].is_string()) {
            throw std::invalid_argument("`source` must be a string when provided");
        }
        const std::string public_source = request_json["source"].get<std::string>();
        if (is_remote_registry_source(public_source)) {
            const std::string normalized_public = remote_registry_source_name(
                parse_remote_registry_source(public_source));
            if (normalized_registry && *normalized_registry != normalized_public) {
                throw std::invalid_argument(
                    "`source` and `registry_source` must identify the same registry");
            }
            normalized_registry = normalized_public;
            request_json["source"] = normalized_public;
        } else if (!local_import && public_source != "local_upload" &&
                   public_source != "local_path" &&
                   public_source != "extra_models_dir") {
            throw std::invalid_argument(
                "Unsupported model source '" + public_source +
                "' (expected 'huggingface', 'modelscope', or a local source)");
        }
    }

    if (normalized_registry) {
        if (!request_json.contains("source") ||
            is_remote_registry_source(request_json["source"].get<std::string>())) {
            request_json["source"] = *normalized_registry;
        }
        request_json["registry_source"] = *normalized_registry;
    }
}

void validate_and_canonicalize_collection(ModelManager& model_manager,
                                          const std::string& model_name,
                                          json& request_json,
                                          bool allow_embedded_models) {
    const std::string recipe = request_json.value("recipe", std::string());
    if (!is_model_collection_recipe(recipe)) {
        return;
    }

    if (request_json.contains("models") && !allow_embedded_models) {
        throw std::invalid_argument(
            "`models` embeds additional model definitions and is not accepted by "
            "the single-model registration endpoint; register components first");
    }

    if (request_json.contains("components")) {
        if (!request_json["components"].is_array()) {
            throw std::invalid_argument("`components` must be an array when provided");
        }
        for (const auto& component : request_json["components"]) {
            if (!component.is_string()) {
                throw std::invalid_argument(
                    "Every collection component must be a string model name");
            }
        }
    }

    if (auto err = model_manager.validate_collection_request(model_name, request_json)) {
        throw std::invalid_argument(*err);
    }

    // Single-definition collections reference models already present in the
    // registry. Exported bundles keep their raw component names because their
    // embedded definitions are resolved by the collection import/download path.
    if (request_json.contains("components") && request_json["components"].is_array() &&
        (!request_json.contains("models") || !request_json["models"].is_array())) {
        for (auto& component : request_json["components"]) {
            component = model_manager.resolve_model_name(component.get<std::string>());
        }
    }
}

// /pull with local_import=true registers files a client already copied into
// dest_path, recording the checkpoint relative to the Hugging Face cache.
void resolve_and_register_local_model(ModelManager& model_manager,
                                      const std::string& dest_path,
                                      const std::string& model_name,
                                      const json& model_data,
                                      const std::string& hf_cache) {
    std::string mmproj = model_data.value("mmproj", "");
    std::string recipe = model_data.value("recipe", "");
    bool vision = model_data.value("vision", false);

    // The backend's ops locate its primary artifact within the imported
    // directory (.gguf / .bin file, genai_config.json dir, …); "" means register
    // the directory itself.
    std::string resolved_checkpoint = backends::ops_for(recipe)->find_imported_checkpoint(dest_path);
    std::string resolved_mmproj;

    if (vision || !mmproj.empty()) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(dest_path)) {
            if (entry.is_regular_file()) {
                std::string filename = entry.path().filename().string();
                std::string filename_lower = filename;
                std::transform(filename_lower.begin(), filename_lower.end(), filename_lower.begin(), ::tolower);

                // Match either the provided mmproj name or any mmproj file
                if (!mmproj.empty() && filename == mmproj) {
                    resolved_mmproj = filename;
                    vision = true;
                    break;
                } else if (filename_lower.find("mmproj") != std::string::npos) {
                    resolved_mmproj = filename;
                    vision = true;
                    break;
                }
            }
        }
    }

    std::string checkpoint_to_register;
    std::filesystem::path hf_cache_path = utils::path_from_utf8(hf_cache);
    if (!resolved_checkpoint.empty()) {
        std::filesystem::path rel = std::filesystem::relative(
            utils::path_from_utf8(resolved_checkpoint), hf_cache_path);
        checkpoint_to_register = utils::path_to_utf8(rel);
    } else {
        std::filesystem::path rel = std::filesystem::relative(
            utils::path_from_utf8(dest_path), hf_cache_path);
        checkpoint_to_register = utils::path_to_utf8(rel);
    }

    LOG(INFO, "Server") << "Registering model with checkpoint: " << checkpoint_to_register << std::endl;

    auto actual_model_data = model_data;
    actual_model_data["checkpoint"] = checkpoint_to_register;
    if (!resolved_mmproj.empty()) {
        actual_model_data["mmproj"] = resolved_mmproj;
    }

    model_manager.register_user_model(model_name, actual_model_data, "local_upload");

    LOG(INFO, "Server") << "Model registered successfully" << std::endl;
}

} // namespace

std::string register_model_definition(ServerContext& ctx, const std::string& model_name,
                                      json& request_json, bool require_definition,
                                      bool allow_embedded_models, bool local_import) {
    ModelManager& model_manager = *ctx.model_manager;
    if (!request_json.is_object()) {
        throw std::invalid_argument("Request body must be a JSON object");
    }

    if (request_json.contains("recipe") && !request_json["recipe"].is_string()) {
        throw std::invalid_argument("`recipe` must be a string when provided");
    }
    const std::string recipe = request_json.value("recipe", std::string());
    if (require_definition && recipe.empty()) {
        throw std::invalid_argument("A non-empty string `recipe` is required");
    }

    if (request_json.contains("checkpoint") && !request_json["checkpoint"].is_string()) {
        throw std::invalid_argument("`checkpoint` must be a string when provided");
    }
    if (request_json.contains("checkpoints")) {
        const auto& checkpoints = request_json["checkpoints"];
        if (!checkpoints.is_object() || !checkpoints.contains("main")) {
            throw std::invalid_argument(
                "If present, `checkpoints` must be an object containing `main`");
        }
        for (const auto& [role, checkpoint] : checkpoints.items()) {
            if (!checkpoint.is_string()) {
                throw std::invalid_argument(
                    "Every `checkpoints` value must be a string; invalid role: " + role);
            }
        }
    }

    const bool has_definition =
        require_definition || !recipe.empty() || request_json.contains("checkpoint") ||
        request_json.contains("checkpoints") || request_json.contains("components") ||
        request_json.contains("models");
    validate_registration_name(model_name, has_definition);

    if (request_json.contains("models") && !allow_embedded_models) {
        throw std::invalid_argument(
            "`models` embeds additional model definitions and is not accepted by "
            "the single-model registration endpoint; register components first");
    }

    normalize_registration_source(request_json, local_import);
    validate_and_canonicalize_collection(model_manager, model_name, request_json,
                                         allow_embedded_models);

    if (!has_definition && !local_import) {
        return model_name;
    }

    if (local_import) {
        std::string hf_cache = model_manager.get_hf_cache_dir();
        std::string model_name_clean = model_name.substr(5);
        std::replace(model_name_clean.begin(), model_name_clean.end(), '/', '-');
        std::string dest_path = hf_cache + "/models--" + model_name_clean;

        LOG(INFO, "Server") << "Local import mode - resolving files in: "
                            << dest_path << std::endl;
        resolve_and_register_local_model(model_manager, dest_path, model_name, request_json,
                                         hf_cache);
    } else {
        model_manager.register_model(
            model_name,
            request_json,
            /*allow_missing_checkpoint=*/require_definition,
            /*replace_existing=*/require_definition);
    }

    return model_manager.get_public_model_name(model_name);
}

} // namespace lemon
