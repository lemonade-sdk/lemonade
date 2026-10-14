#include "lemon/server/model_json.h"

#include <vector>

#include "lemon/alias_manager.h"
#include "lemon/model_registry.h"
#include "lemon/router.h"

namespace lemon {

using json = nlohmann::json;

namespace {

// Collection components are normally leaf models, but nothing prevents registering a
// collection as a component of another, even cyclically, so embedding stops here.
constexpr int kMaxCollectionEmbedDepth = 3;

std::string public_source(const ModelInfo& info) {
    return info.source.empty()
        ? remote_registry_source_name(parse_remote_registry_source(info.registry_source))
        : info.source;
}

} // namespace

ModelJson::ModelJson(ModelManager* model_manager, Router* router, AliasManager* alias_manager)
    : model_manager_(model_manager), router_(router), alias_manager_(alias_manager) {}

int64_t ModelJson::resolve_context_length(const std::string& model_id, const ModelInfo& info) const {
    // ctx_size stores -1 for "size this automatically", so only a positive
    // value answers; anything else falls through to the next source.
    auto ctx_size_of = [](const RecipeOptions& options) -> int64_t {
        const json ctx_json = options.get_option("ctx_size");
        return ctx_json.is_number() ? ctx_json.get<int64_t>() : 0;
    };

    if (router_) {
        std::string loaded_name = model_id;
        if (alias_manager_) {
            if (auto target = alias_manager_->resolve_alias(model_id)) {
                loaded_name = *target;
            }
        }
        const int64_t loaded_ctx = ctx_size_of(router_->get_model_recipe_options(loaded_name));
        if (loaded_ctx > 0) {
            return loaded_ctx;
        }

        const RecipeOptions no_request_options(info.recipe, json::object());
        const int64_t configured_ctx =
            ctx_size_of(router_->resolve_effective_options(info, no_request_options));
        if (configured_ctx > 0) {
            return configured_ctx;
        }
    }

    return info.max_context_window > 0 ? info.max_context_window : 0;
}

json ModelJson::to_json(const std::string& model_id, const ModelInfo& info, int depth) const {
    std::vector<std::string> public_components;
    public_components.reserve(info.components.size());
    for (const auto& component : info.components) {
        public_components.push_back(model_manager_->get_public_model_name(component));
    }
    json model_json = {
        {"id", model_id},
        {"object", "model"},
        {"created", 1234567890},
        {"owned_by", "lemonade"},
        {"checkpoint", info.checkpoint()},
        {"checkpoints", info.checkpoints},
        {"recipe", info.recipe},
        {"downloaded", info.downloaded},
        {"update_available", info.update_available},
        {"suggested", info.suggested},
        {"source", public_source(info)},
        {"registry_source", remote_registry_source_name(
            parse_remote_registry_source(info.registry_source))},
        {"labels", info.labels},
        {"components", public_components},
        {"recipe_options", info.recipe_options.to_json()},
    };

    // The Model Manager buckets cloud models by provider. Local models omit the
    // field rather than carry an empty one.
    if (!info.cloud_provider.empty()) {
        model_json["cloud_provider"] = info.cloud_provider;
    }

    if (info.size > 0.0) {
        model_json["size"] = info.size;
    }

    if (info.max_context_window > 0) {
        model_json["max_context_window"] = info.max_context_window;
    }

    // OpenAI-compatible clients use context_length to set token limits.
    const int64_t context_length = resolve_context_length(model_id, info);
    if (context_length > 0) {
        model_json["context_length"] = context_length;
    }

    if (info.max_output_tokens > 0) {
        model_json["max_output_tokens"] = info.max_output_tokens;
        model_json["max_completion_tokens"] = info.max_output_tokens;
    }

    if (info.cost_input_per_million >= 0) {
        model_json["cost_input_per_million"] = info.cost_input_per_million;
    }
    if (info.cost_output_per_million >= 0) {
        model_json["cost_output_per_million"] = info.cost_output_per_million;
    }

    if (!info.system_prompt.empty()) {
        model_json["system_prompt"] = info.system_prompt;
    }

    auto audio_defaults = info.extras.find("audio_defaults");
    if (audio_defaults != info.extras.end() && audio_defaults->second.is_object()) {
        model_json["audio_defaults"] = audio_defaults->second;
    }

    if (info.image_defaults.has_defaults) {
        json img_def = {
            {"steps", info.image_defaults.steps},
            {"cfg_scale", info.image_defaults.cfg_scale},
            {"width", info.image_defaults.width},
            {"height", info.image_defaults.height}
        };
        if (!info.image_defaults.sampling_method.empty())
            img_def["sampling_method"] = info.image_defaults.sampling_method;
        if (info.image_defaults.flow_shift > 0.0f)
            img_def["flow_shift"] = info.image_defaults.flow_shift;
        if (info.image_defaults.soft_pixel_cap > 0)
            img_def["soft_pixel_cap"] = info.image_defaults.soft_pixel_cap;
        model_json["image_defaults"] = img_def;
    }

    if (is_router_collection_recipe(info.recipe)) {
        // The parser requires a root "version"; surface it alongside "routing"
        // so an exported router collection can be re-imported through /pull.
        auto version_it = info.extras.find("version");
        if (version_it != info.extras.end()) {
            model_json["version"] = version_it->second;
        }
        auto routing_it = info.extras.find("routing");
        if (routing_it != info.extras.end() && routing_it->second.is_object()) {
            model_json["routing"] = routing_it->second;
        }
    }

    // Collections embed each component's full model object, in component order, so an
    // exported collection imports elsewhere without its components' definitions.
    if (is_model_collection_recipe(info.recipe) && depth < kMaxCollectionEmbedDepth) {
        json component_models = json::array();
        for (const auto& component : info.components) {
            if (!model_manager_->model_exists(component)) {
                continue;
            }
            auto comp_info = model_manager_->get_model_info(component);
            component_models.push_back(to_json(
                model_manager_->get_public_model_name(component), comp_info, depth + 1));
        }
        model_json["models"] = component_models;
    }

    return model_json;
}

json ModelJson::status_page_json(const std::string& model_id, const ModelInfo& info) const {
    std::vector<std::string> public_components;
    public_components.reserve(info.components.size());
    for (const auto& component : info.components) {
        public_components.push_back(model_manager_->get_public_model_name(component));
    }
    json model_json = {
        {"model_name", model_id},
        {"checkpoint", info.checkpoint()},
        {"recipe", info.recipe},
        {"labels", info.labels},
        {"suggested", info.suggested},
        {"source", public_source(info)},
        {"registry_source", remote_registry_source_name(
            parse_remote_registry_source(info.registry_source))},
        {"components", public_components},
        {"mmproj", info.mmproj()}
    };
    if (info.size > 0.0) {
        model_json["size"] = info.size;
    }
    return model_json;
}

nlohmann::ordered_json ModelJson::schema() {
    return nlohmann::ordered_json::parse(R"json({
        "type": "object",
        "required": ["id", "object", "created", "owned_by", "checkpoint", "checkpoints", "recipe",
                     "downloaded", "update_available", "suggested", "source", "registry_source",
                     "labels", "components", "recipe_options"],
        "properties": {
            "id": {"type": "string", "description": "Model identifier (used for loading and inference requests)"},
            "created": {"type": "integer", "description": "Unix timestamp of when the model entry was created"},
            "object": {"const": "model", "description": "Type of object, always `\"model\"`"},
            "owned_by": {"const": "lemonade", "description": "Owner of the model, always `\"lemonade\"`"},
            "checkpoint": {"type": "string", "description": "Full checkpoint identifier on Hugging Face"},
            "recipe": {"type": "string", "description": "Backend/device recipe used to load the model (e.g., `\"ryzenai-llm\"`, `\"llamacpp\"`, `\"flm\"`)"},
            "size": {"type": "number", "description": "Model size in GB (omitted for models without size information)"},
            "max_context_window": {"type": "integer", "description": "Optional integer indicating the maximum model-supported text context discovered from local static metadata. Currently populated for downloaded GGUF/llama.cpp models and installed FLM text-context models."},
            "context_length": {"type": "integer", "description": "Number of tokens the model can handle in one request. Uses the loaded value when the model is running, the configured `ctx_size`, or the discovered cloud provider context length otherwise (omitted when neither is known)."},
            "max_output_tokens": {"type": "integer", "description": "Optional integer indicating the maximum output tokens the model can generate in a single completion, when reported by cloud providers."},
            "max_completion_tokens": {"type": "integer", "description": "Optional integer alias for `max_output_tokens` adhering to OpenAI convention, when reported by cloud providers."},
            "downloaded": {"type": "boolean", "description": "Boolean indicating if the model is downloaded and available locally"},
            "update_available": {"type": "boolean", "description": "Boolean indicating a newer commit exists on HuggingFace for this model. Only set for downloaded HF-backed models. `false` otherwise."},
            "suggested": {"type": "boolean", "description": "Boolean indicating if the model is recommended for general use"},
            "labels": {"type": "array", "items": {"type": "string"}, "description": "Array of tags describing the model's capabilities and characteristics. See [Model Labels](./openai.md#model-labels) for the full list."},
            "image_defaults": {
                "type": "object",
                "description": "(Image models only) Default generation parameters for the model:",
                "properties": {
                    "steps": {"type": "integer", "description": "Number of inference steps (e.g., 4 for turbo models, 20 for standard models)"},
                    "cfg_scale": {"type": "number", "description": "Classifier-free guidance scale (e.g., 1.0 for turbo models, 7.5 for standard models)"},
                    "width": {"type": "integer", "description": "Default image width in pixels"},
                    "height": {"type": "integer", "description": "Default image height in pixels"},
                    "sampling_method": {"type": "string"},
                    "flow_shift": {"type": "number"},
                    "soft_pixel_cap": {"type": "integer", "description": "Largest output area, in pixels, that images/edits infers from the first reference image when size is omitted."}
                }
            },
            "components": {"type": "array", "items": {"type": "string"}, "description": "(Omni collections only, `recipe: \"collection.omni\"`) Ordered array of the component model names that make up the collection"},
            "models": {"type": "array", "items": {"type": "object"},
                       "description": "(Omni collections only) Ordered array embedding each component's full model object (same shape as the entries in this list), parallel to `components`. This makes a collection's `/v1/models/{id}` response self-contained — exporting it produces a file that can be imported elsewhere via [`/v1/pull`](./lemonade.md#post-v1pull)"},
            "checkpoints": {"type": "object", "description": "Every checkpoint by role, such as main, mmproj, draft, text_encoder or vae.",
                            "additionalProperties": {"type": "string"}},
            "source": {"type": "string", "description": "Where the model came from: a registry name, local_upload, local_path or extra_models_dir."},
            "registry_source": {"type": "string", "description": "Remote registry the checkpoint downloads from: huggingface or modelscope."},
            "recipe_options": {"type": "object", "description": "Options saved for this model in recipe_options.json."},
            "cloud_provider": {"type": "string", "description": "Cloud models only: the provider that serves the model."},
            "cost_input_per_million": {"type": "number", "description": "Cloud models only: USD per million input tokens, when the provider reports it."},
            "cost_output_per_million": {"type": "number", "description": "Cloud models only: USD per million output tokens, when the provider reports it."},
            "system_prompt": {"type": "string", "description": "Omni collections only: the collection's system prompt override."},
            "audio_defaults": {"type": "object", "description": "Audio models only: default generation parameters."},
            "version": {"description": "Router collections only: the policy document's version."},
            "routing": {"type": "object", "description": "Router collections only: the routing policy."}
        }
    })json");
}

} // namespace lemon
