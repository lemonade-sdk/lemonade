#include <lemon/utils/aixlog.hpp>

#include "lemon/backends/backend_descriptor_registry.h"
#include "lemon/model_manager.h"
#include "lemon/model_types.h"
#include "lemon/server/api_route.h"
#include "lemon/server/gateway_conversion.h"
#include "lemon/server/shared_args.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

json build_ollama_capabilities(const ModelInfo& info) {
    json capabilities = json::array();
    ModelType model_type = get_model_type_from_labels(info.labels);

    // The same check /v1/systemone makes. Like Ollama, a decision model reports only
    // `decision` and the inputs it takes, which sends the Ollama CLI to /v1/systemone.
    if (model_type == ModelType::CLASSIFICATION && has_label(info.labels, "systemone") &&
        backends::recipe_gates_mode_on(info.recipe, "systemone")) {
        capabilities.push_back("decision");
        if (has_label(info.labels, "vision")) {
            capabilities.push_back("vision");
        }
        return capabilities;
    }

    if (model_type == ModelType::LLM) {
        capabilities.push_back("completion");
    } else if (model_type == ModelType::EMBEDDING) {
        capabilities.push_back("embedding");
    } else if (ollama_output(info.labels) == OllamaOutput::Image) {
        capabilities.push_back("image");
    } else if (model_type == ModelType::TTS) {
        capabilities.push_back("speech");
    }

    if (has_label(info.labels, "tool-calling") || has_label(info.labels, "tools")) {
        capabilities.push_back("tools");
    }

    if (has_label(info.labels, "vision")) {
        capabilities.push_back("vision");
    }

    // Ollama clients send audio inside `images`; only llama.cpp has been shown to hear it there.
    if (has_label(info.labels, "chat-transcription") && info.recipe == "llamacpp") {
        capabilities.push_back("audio");
    }

    if (has_label(info.labels, "reasoning")) {
        capabilities.push_back("thinking");
    }

    return capabilities;
}

class ShowRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "ollama.show";
        s.methods = {"POST"};
        s.paths = {"/api/show"};
        s.prefixes = Prefixes::Root;
        s.summary = "Describe a model";
        s.description = "Describes one registered model in Ollama's format.";
        s.args = {ollama::management::kModel};

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = ordered_json::parse(R"({
            "type": "object",
            "required": ["modelfile", "parameters", "template", "details", "capabilities", "model_info"],
            "properties": {
                "modelfile": {"type": "string", "description": "A `FROM` line naming the checkpoint."},
                "parameters": {"type": "string", "description": "`num_ctx` followed by the model's configured context size, which is `-1` when Lemonade sizes the context at load time."},
                "template": {"type": "string", "description": "Always empty."},
                "details": {},
                "capabilities": {"type": "array", "items": {"enum": ["completion", "embedding", "image", "speech", "decision", "tools", "vision", "audio", "thinking"]}, "description": "From the model's labels: `completion` for chat models, `embedding` for embedding models, `image` for image generation models, `speech` for text-to-speech models, plus `tools`, `vision` and `thinking` for the `tool-calling` (or `tools`), `vision` and `reasoning` labels, and `audio` for llama.cpp models with the `chat-transcription` label. A SystemOne decision model reports only `decision`, plus `vision` when it has that label."},
                "model_info": {"type": "object", "description": "`general.architecture` (the recipe) and `<recipe>.context_length` (the context size in `parameters`); the other keys are always `0`."}
            }
        })");
        response.schema["properties"]["details"] = ollama_details_schema();
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.args = ordered_json::parse(R"({"model": "Qwen3-0.6B-GGUF"})");
        s.examples = {example};
        s.errors = {kBodyNotJson, kArgMissing, kArgInvalid, kOllamaModelNotFound, ApiRoute::kUnexpectedException};
        s.request_format = RequestFormat::Json;
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        try {
            std::string name = strip_latest_tag(req.arg(ollama::management::kModel).get<std::string>());
            if (!ollama_model_registered(*ctx_.model_manager, name, res)) {
                return;
            }

            auto info = ctx_.model_manager->get_model_info(name);
            int ctx_size = info.recipe_options.get_option("ctx_size");

            json response = {
                {"modelfile", "# Modelfile generated by Lemonade\nFROM " + info.checkpoint()},
                {"parameters", "num_ctx " + std::to_string(ctx_size)},
                {"template", ""},
                {"details", ollama_details(name, info.recipe, info.checkpoint())},
                {"capabilities", build_ollama_capabilities(info)},
                {"model_info", {
                    {"general.architecture", info.recipe},
                    {info.recipe + ".context_length", ctx_size},
                    {"general.file_type", 0},
                    {"general.parameter_count", 0},
                    {"general.quantization_version", 0}
                }}
            };

            res.set_content(response.dump(), "application/json");

        } catch (const std::exception& e) {
            LOG(ERROR, "OllamaApi") << "Error in /api/show: " << e.what() << std::endl;
            write_plain_error(res, ApiRoute::kUnexpectedException, e.what());
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_ollama_show_route(ServerContext& ctx) {
    return std::make_unique<ShowRoute>(ctx);
}

} // namespace lemon
