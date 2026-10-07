#include <string>

#include <lemon/utils/aixlog.hpp>

#include "lemon/error_types.h"
#include "lemon/model_manager.h"
#include "lemon/model_types.h"
#include "lemon/router.h"
#include "lemon/server/api_route.h"
#include "lemon/server/gateway_conversion.h"
#include "lemon/server/model_loader.h"

namespace lemon {
namespace {

using json = nlohmann::json;

json convert_ollama_to_openai_completion(const json& ollama_request) {
    json openai_req;

    std::string model = strip_latest_tag(ollama_request.value("model", ""));
    openai_req["model"] = model;

    if (ollama_request.contains("prompt")) {
        openai_req["prompt"] = ollama_request["prompt"];
    }

    map_ollama_options(ollama_request, openai_req);

    openai_req["stream"] = false;

    return openai_req;
}

// /api/generate also serves image models, answering with the image as base64.
void generate_image(Router& router, const json& request_json, httplib::Response& res,
                    const std::string& model) {
    try {
        LOG(INFO, "OllamaApi") << "POST /api/generate - Image generation (model: " << model << ")" << std::endl;

        std::string prompt = request_json.value("prompt", "");

        // Each parameter is read from the top level first, then from options.
        int width = 512;
        int height = 512;
        int steps = 0;
        double cfg_scale = 0.0;
        int seed = -1;

        if (request_json.contains("width")) {
            width = request_json["width"].get<int>();
        } else if (request_json.contains("options") && request_json["options"].contains("width")) {
            width = request_json["options"]["width"].get<int>();
        }

        if (request_json.contains("height")) {
            height = request_json["height"].get<int>();
        } else if (request_json.contains("options") && request_json["options"].contains("height")) {
            height = request_json["options"]["height"].get<int>();
        }

        if (request_json.contains("steps")) {
            steps = request_json["steps"].get<int>();
        } else if (request_json.contains("options") && request_json["options"].contains("steps")) {
            steps = request_json["options"]["steps"].get<int>();
        }

        if (request_json.contains("cfg_scale")) {
            cfg_scale = request_json["cfg_scale"].get<double>();
        } else if (request_json.contains("options") && request_json["options"].contains("cfg_scale")) {
            cfg_scale = request_json["options"]["cfg_scale"].get<double>();
        }

        if (request_json.contains("seed")) {
            seed = request_json["seed"].get<int>();
        } else if (request_json.contains("options") && request_json["options"].contains("seed")) {
            seed = request_json["options"]["seed"].get<int>();
        }

        json openai_req;
        openai_req["model"] = model;
        openai_req["prompt"] = prompt;
        openai_req["size"] = std::to_string(width) + "x" + std::to_string(height);
        openai_req["response_format"] = "b64_json";

        if (steps > 0) {
            openai_req["steps"] = steps;
        }
        if (cfg_scale > 0.0) {
            openai_req["cfg_scale"] = cfg_scale;
        }
        if (seed >= 0) {
            openai_req["seed"] = seed;
        }

        auto openai_response = router.image_generations(openai_req);

        if (write_ollama_backend_error(openai_response, res)) return;

        json ollama_res;
        ollama_res["model"] = model;
        ollama_res["created_at"] = "2024-01-01T00:00:00Z";
        ollama_res["response"] = "";
        ollama_res["done"] = true;

        if (openai_response.contains("data") && !openai_response["data"].empty()) {
            ollama_res["image"] = openai_response["data"][0].value("b64_json", "");
        } else {
            ollama_res["image"] = "";
        }

        res.set_content(ollama_res.dump(), "application/json");

    } catch (const std::exception& e) {
        LOG(ERROR, "OllamaApi") << "Error in image generation: " << e.what() << std::endl;
        res.status = 500;
        json error = {{"error", std::string(e.what())}};
        res.set_content(error.dump(), "application/json");
    }
}

json generate_response_schema(bool line) {
    json schema = json::parse(R"({
        "type": "object",
        "required": ["model", "created_at", "done"],
        "properties": {
            "model": {"type": "string"},
            "created_at": {"type": "string", "description": "Always 2024-01-01T00:00:00Z."},
            "response": {"type": "string", "description": "Generated text. Empty for an image model."},
            "image": {"type": "string", "description": "Image models only: the generated image, base64-encoded."},
            "done": {"type": "boolean"},
            "done_reason": {"type": "string", "description": "stop, length, or unload after an unload request."},
            "context": {"type": "array", "description": "Always empty."},
            "prompt_eval_count": {"type": "integer", "description": "Prompt tokens."},
            "eval_count": {"type": "integer", "description": "Generated tokens."},
            "total_duration": {"type": "integer", "description": "Always 0."},
            "load_duration": {"type": "integer", "description": "Always 0."},
            "prompt_eval_duration": {"type": "integer", "description": "Always 0."},
            "eval_duration": {"type": "integer", "description": "Always 0."}
        }
    })");
    if (line) {
        schema["description"] = "Each line carries the next piece of text with done=false. When the "
                                "text ends, a line with done=true and the real done_reason follows, "
                                "then a last done=true line with the token counts and done_reason "
                                "stop.";
    }
    return schema;
}

class GenerateRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "ollama.generate";
        s.methods = {"POST"};
        s.paths = {"/api/generate"};
        s.prefixes = Prefixes::Root;
        s.summary = "Text completion, or image generation for image models";
        s.description =
            "Continues a prompt, loading the model on first use. Lemonade runs it as an OpenAI "
            "text completion and converts the result. Naming an image model generates an image "
            "instead.";
        s.notes = {
            "Ollama streams by default: without `\"stream\": false` the response is "
            "newline-delimited JSON. Image generation never streams.",
            "An empty `prompt` with `\"keep_alive\": 0` unloads the model instead, answering "
            "with `done_reason: \"unload\"`.",
            "An unknown model answers `404` with `model '<name>' not found, try pulling it "
            "first`.",
        };
        s.args = {
            {"model", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
             "Model to run; loaded on first use. A `:latest` tag is ignored."},
            {"prompt", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
             "Text to continue, or the image to generate for an image model."},
            {"stream", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
             "Stream newline-delimited JSON as tokens are generated. Defaults to `true`."},
            {"options", ArgIn::JsonBody, {{"type", "object"}}, false, Support::Available,
             "Sampling options: `temperature`, `top_p`, `seed`, `stop`, `num_predict` (maximum "
             "generated tokens) and `repeat_penalty`. These keys are also accepted at the top "
             "level, where they win. `num_ctx` sets the context size when this request loads "
             "the model. Image models read `width`, `height`, `steps`, `cfg_scale` and `seed` "
             "here."},
            {"width", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
             "Image models only: image width in pixels. Defaults to 512."},
            {"height", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
             "Image models only: image height in pixels. Defaults to 512."},
            {"steps", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
             "Image models only: inference steps. Defaults to the model's own."},
            {"cfg_scale", ArgIn::JsonBody, {{"type", "number"}}, false, Support::Available,
             "Image models only: classifier-free guidance scale. Defaults to the model's own."},
            {"seed", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
             "Random seed for reproducible output."},
            {"keep_alive", ArgIn::JsonBody, json::object(), false, Support::Available,
             "`0` with an empty `prompt` unloads the model. Other values are ignored."},
            {"ctx_size", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
             "Lemonade extension: context size to load the model with, when this request loads "
             "it. Wins over `options.num_ctx`."},
            {"system", ArgIn::JsonBody, {{"type", "string"}}, false, Support::NotAvailable,
             "System prompt."},
            {"format", ArgIn::JsonBody, {{"type", "string"}}, false, Support::NotAvailable,
             "Output format constraint."},
        };

        RouteResponse full;
        full.format = ResponseFormat::Json;
        full.schema = generate_response_schema(false);
        full.example = json::parse(R"({
            "model": "Qwen3-0.6B-GGUF",
            "prompt": "The capital of France is",
            "options": {"num_predict": 16},
            "stream": false
        })");

        RouteResponse stream;
        stream.format = ResponseFormat::JsonLines;
        stream.schema = generate_response_schema(true);
        stream.example = json::parse(R"({
            "model": "Qwen3-0.6B-GGUF",
            "prompt": "The capital of France is",
            "options": {"num_predict": 16},
            "stream": true
        })");

        s.responses = {full, stream};
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        try {
            auto request_json = json::parse(req.http.body);

            std::string model = strip_latest_tag(request_json.value("model", ""));
            if (model.empty()) {
                res.status = 400;
                res.set_content(R"({"error":"model is required"})", "application/json");
                return;
            }

            std::string prompt = request_json.value("prompt", "");
            if (prompt.empty() && request_json.contains("keep_alive") &&
                request_json["keep_alive"] == 0) {
                LOG(INFO, "OllamaApi") << "POST /api/generate - Unloading model: " << model << std::endl;
                try {
                    ctx_.router->unload_model(model);
                } catch (...) {
                    // Unloading a model that is not loaded is not an error here.
                }
                json ollama_res = {
                    {"model", model},
                    {"created_at", "2024-01-01T00:00:00Z"},
                    {"response", ""},
                    {"done", true},
                    {"done_reason", "unload"}
                };
                res.set_content(ollama_res.dump(), "application/json");
                return;
            }

            try {
                ctx_.model_loader->ensure_loaded(model,
                                                 gateway_load_options(request_json));
            } catch (const RouterResidencyConflictException& e) {
                write_ollama_residency_conflict(e, res);
                return;
            } catch (const std::exception& e) {
                res.status = 404;
                json error = {{"error", "model '" + model + "' not found, try pulling it first"}};
                res.set_content(error.dump(), "application/json");
                return;
            }

            auto model_info = ctx_.model_manager->get_model_info(model);
            ModelType model_type = get_model_type_from_labels(model_info.labels);

            if (model_type == ModelType::IMAGE) {
                generate_image(*ctx_.router, request_json, res, model);
                return;
            }

            bool stream = request_json.value("stream", true);

            auto openai_req = convert_ollama_to_openai_completion(request_json);

            if (stream) {
                LOG(INFO, "OllamaApi") << "POST /api/generate - Streaming (model: " << model << ")" << std::endl;

                openai_req["stream"] = true;
                req.body = openai_req;
                Router* router = ctx_.router;
                stream_response(req, res, [router, model](const std::string& body, httplib::DataSink& sink) {
                    stream_sse_to_ndjson(body, sink,
                        [&model](const json& openai_chunk) -> json {
                            json ollama_chunk = {
                                {"model", model}, {"created_at", "2024-01-01T00:00:00Z"},
                                {"done", false}
                            };
                            if (openai_chunk.contains("choices") && !openai_chunk["choices"].empty()) {
                                const auto& choice = openai_chunk["choices"][0];
                                if (choice.contains("text"))
                                    ollama_chunk["response"] = choice["text"];
                                else if (choice.contains("delta") && choice["delta"].contains("content"))
                                    ollama_chunk["response"] = choice["delta"]["content"];
                                else
                                    ollama_chunk["response"] = "";
                                if (choice.contains("finish_reason") && !choice["finish_reason"].is_null()) {
                                    ollama_chunk["done"] = true;
                                    ollama_chunk["done_reason"] = choice["finish_reason"];
                                }
                            }
                            return ollama_chunk;
                        },
                        [&model](int prompt_eval_count, int eval_count) -> json {
                            return {
                                {"model", model}, {"created_at", "2024-01-01T00:00:00Z"},
                                {"response", ""}, {"done", true}, {"done_reason", "stop"},
                                {"context", json::array()},
                                {"total_duration", 0}, {"load_duration", 0},
                                {"prompt_eval_count", prompt_eval_count}, {"prompt_eval_duration", 0},
                                {"eval_count", eval_count}, {"eval_duration", 0}
                            };
                        },
                        [router](const std::string& openai_body, httplib::DataSink& s) {
                            router->completion_stream(openai_body, s);
                        });
                }, "application/x-ndjson");
            } else {
                LOG(INFO, "OllamaApi") << "POST /api/generate - Non-streaming (model: " << model << ")" << std::endl;

                auto openai_response = ctx_.router->completion(openai_req);

                if (write_ollama_backend_error(openai_response, res)) return;

                json ollama_res;
                ollama_res["model"] = model;
                ollama_res["created_at"] = "2024-01-01T00:00:00Z";
                ollama_res["done"] = true;
                ollama_res["done_reason"] = "stop";

                if (openai_response.contains("choices") && !openai_response["choices"].empty()) {
                    const auto& choice = openai_response["choices"][0];
                    ollama_res["response"] = choice.value("text", "");
                } else {
                    ollama_res["response"] = "";
                }

                if (openai_response.contains("usage")) {
                    const auto& usage = openai_response["usage"];
                    ollama_res["prompt_eval_count"] = usage.value("prompt_tokens", 0);
                    ollama_res["eval_count"] = usage.value("completion_tokens", 0);
                }

                ollama_res["total_duration"] = 0;
                ollama_res["load_duration"] = 0;
                ollama_res["prompt_eval_duration"] = 0;
                ollama_res["eval_duration"] = 0;
                ollama_res["context"] = json::array();

                res.set_content(ollama_res.dump(), "application/json");
            }

        } catch (const std::exception& e) {
            LOG(ERROR, "OllamaApi") << "Error in /api/generate: " << e.what() << std::endl;
            res.status = 500;
            json error = {{"error", std::string(e.what())}};
            res.set_content(error.dump(), "application/json");
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_ollama_generate_route(ServerContext& ctx) {
    return std::make_unique<GenerateRoute>(ctx);
}

} // namespace lemon
