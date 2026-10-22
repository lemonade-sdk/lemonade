#include <string>

#include <lemon/utils/aixlog.hpp>

#include "lemon/model_manager.h"
#include "lemon/model_types.h"
#include "lemon/router.h"
#include "lemon/server/api_route.h"
#include "lemon/server/gateway_conversion.h"
#include "lemon/server/openai_args.h"
#include "lemon/server/shared_args.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

const RouteArg kPrompt{"prompt", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
                       "Text to continue, or the image to generate for an image model."};
const RouteArg kOptions{"options", ArgIn::JsonBody, {{"type", "object"}}, false, Support::Partial,
                        "Sampling options: `temperature`, `top_p`, `stop`, `num_predict` (maximum "
                        "generated tokens), `repeat_penalty`, which is sent as OpenAI's "
                        "`frequency_penalty`, and the `seed`, `width`, `height`, `steps` and `cfg_scale` "
                        "documented below. A key given at the top level wins. `num_ctx` sets the context "
                        "size when this request loads the model, unless `ctx_size` is sent. Other keys "
                        "are ignored."};
const RouteArg kWidth{"width", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
                      "Image models only: image width in pixels. Defaults to `512`."};
const RouteArg kHeight{"height", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
                       "Image models only: image height in pixels. Defaults to `512`."};
const RouteArg kSteps{"steps", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
                      "Image models only: inference steps. Defaults to the model's own."};
const RouteArg kCfgScale{"cfg_scale", ArgIn::JsonBody, {{"type", "number"}}, false, Support::Available,
                         "Image models only: classifier-free guidance scale. Defaults to the model's own."};
const RouteArg kSeed{"seed", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
                     "Random seed for reproducible output."};
const RouteArg kSystem{"system", ArgIn::JsonBody, {{"type", "string"}}, false, Support::NotAvailable,
                       "System prompt. Ignored."};
const RouteArg kFormat{"format", ArgIn::JsonBody, {{"type", json::array({"string", "object"})}}, false,
                       Support::NotAvailable, "Output format constraint. Ignored."};

json convert_ollama_to_openai_completion(const RouteRequest& req) {
    json openai_req;

    std::string model = strip_latest_tag(req.value(gateway::kModel, ""));
    openai_req[model_loading::kModel.name] = model;

    if (req.has(kPrompt)) {
        openai_req[openai::completions::kPrompt.name] = req.arg(kPrompt);
    }

    map_ollama_options(req.arg(kOptions), req.body, openai_req);

    openai_req[common::kStream.name] = false;

    return openai_req;
}

// /api/generate also serves image models, answering with the image as base64.
void generate_image(Router& router, const RouteRequest& req, httplib::Response& res,
                    const std::string& model) {
    try {
        LOG(INFO, "OllamaApi") << "POST /api/generate - Image generation (model: " << model << ")" << std::endl;

        std::string prompt = req.value(kPrompt, "");

        // Each parameter is read from the top level first, then from options.
        int width = 512;
        int height = 512;
        int steps = 0;
        double cfg_scale = 0.0;
        int seed = -1;

        if (req.has(kWidth)) {
            width = req.arg(kWidth).get<int>();
        } else if (req.has(kOptions) && req.arg(kOptions).contains("width")) {
            width = req.arg(kOptions)["width"].get<int>();
        }

        if (req.has(kHeight)) {
            height = req.arg(kHeight).get<int>();
        } else if (req.has(kOptions) && req.arg(kOptions).contains("height")) {
            height = req.arg(kOptions)["height"].get<int>();
        }

        if (req.has(kSteps)) {
            steps = req.arg(kSteps).get<int>();
        } else if (req.has(kOptions) && req.arg(kOptions).contains("steps")) {
            steps = req.arg(kOptions)["steps"].get<int>();
        }

        if (req.has(kCfgScale)) {
            cfg_scale = req.arg(kCfgScale).get<double>();
        } else if (req.has(kOptions) && req.arg(kOptions).contains("cfg_scale")) {
            cfg_scale = req.arg(kOptions)["cfg_scale"].get<double>();
        }

        if (req.has(kSeed)) {
            seed = req.arg(kSeed).get<int>();
        } else if (req.has(kOptions) && req.arg(kOptions).contains("seed")) {
            seed = req.arg(kOptions)["seed"].get<int>();
        }

        json openai_req;
        openai_req[model_loading::kModel.name] = model;
        openai_req[openai::images::kPrompt.name] = prompt;
        openai_req[openai::images::kSize.name] = std::to_string(width) + "x" + std::to_string(height);
        openai_req[openai::images::kResponseFormat.name] = "b64_json";

        if (steps > 0) {
            openai_req[inference::kSteps.name] = steps;
        }
        if (cfg_scale > 0.0) {
            openai_req[openai::images::kCfgScale.name] = cfg_scale;
        }
        if (seed >= 0) {
            openai_req[openai::images::kSeed.name] = seed;
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
        write_plain_error(res, ApiRoute::kUnexpectedException, e.what());
    }
}

ordered_json generate_response_schema(bool line) {
    ordered_json schema = ordered_json::parse(R"({
        "type": "object",
        "required": ["model", "created_at", "done"],
        "properties": {
            "model": {"type": "string", "description": "The model that generated the response."},
            "created_at": {"type": "string", "description": "Always `2024-01-01T00:00:00Z`."},
            "response": {"type": "string", "description": "Generated text. Empty for an image model."},
            "image": {"type": "string", "description": "Image models only: the generated image, base64-encoded."},
            "done": {"type": "boolean", "description": "`true` on a response that is not streamed, and on the last two lines of a stream."},
            "done_reason": {"type": "string", "description": "`stop`, or `unload` after an unload request."},
            "context": {"type": "array", "description": "Always empty."},
            "prompt_eval_count": {"type": "integer", "description": "Prompt tokens."},
            "eval_count": {"type": "integer", "description": "Generated tokens."},
            "total_duration": {"type": "integer", "description": "Always `0`."},
            "load_duration": {"type": "integer", "description": "Always `0`."},
            "prompt_eval_duration": {"type": "integer", "description": "Always `0`."},
            "eval_duration": {"type": "integer", "description": "Always `0`."}
        }
    })");
    if (line) {
        schema["description"] = "Each line carries the next piece of text with `done: false`. When "
                                "the text ends, a line with `done: true` and the real `done_reason` "
                                "follows, then a last `done: true` line with the token counts and "
                                "`done_reason` `stop`.";
        schema["properties"]["done_reason"]["description"] =
            "`stop`, `length`, or `unload` after an unload request.";
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
        s.summary = "Continue a prompt or generate an image";
        s.description =
            "Continues a prompt, loading the model on first use. Lemonade runs it as an OpenAI "
            "text completion and converts the result. Naming an image model generates an image "
            "instead.";
        s.notes = {
            "Image generation never streams.",
        };
        s.args = {
            gateway::kModel, kPrompt, ollama::kStream, kOptions, kWidth, kHeight, kSteps, kCfgScale,
            kSeed, ollama::kKeepAlive, model_loading::kCtxSize, kSystem, kFormat
        };

        RouteResponse full;
        full.format = ResponseFormat::Json;
        full.schema = generate_response_schema(false);

        RouteResponse stream;
        stream.format = ResponseFormat::JsonLines;
        stream.schema = generate_response_schema(true);

        s.responses = {full, stream};

        RouteExample full_example;
        full_example.name = "non-streaming";
        full_example.format = ResponseFormat::Json;
        full_example.args = ordered_json::parse(R"({
            "model": "Qwen3-0.6B-GGUF",
            "prompt": "The capital of France is",
            "options": {"num_predict": 16},
            "stream": false
        })");

        RouteExample stream_example;
        stream_example.name = "streaming";
        stream_example.format = ResponseFormat::JsonLines;
        stream_example.args = ordered_json::parse(R"({
            "model": "Qwen3-0.6B-GGUF",
            "prompt": "The capital of France is",
            "options": {"num_predict": 16},
            "stream": true
        })");

        s.examples = {full_example, stream_example};
        s.errors = join_errors({{kBodyNotJson, kArgMissing, kArgInvalid}, gateway_load_errors(),
                                {kOllamaBackendError, ApiRoute::kUnexpectedException}});
        s.request_format = RequestFormat::Json;
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        try {
            std::string model = strip_latest_tag(req.arg(gateway::kModel).get<std::string>());

            std::string prompt = req.value(kPrompt, "");
            if (prompt.empty() && req.has(ollama::kKeepAlive) && req.arg(ollama::kKeepAlive) == 0) {
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

            if (!gateway_ensure_loaded(*ctx_.model_loader, model,
                                       gateway_load_options(req.arg(kOptions), req.arg(model_loading::kCtxSize)),
                                       ErrorFormat::Plain, res)) {
                return;
            }

            auto model_info = ctx_.model_manager->get_model_info(model);
            ModelType model_type = get_model_type_from_labels(model_info.labels);

            if (model_type == ModelType::IMAGE) {
                generate_image(*ctx_.router, req, res, model);
                return;
            }

            bool stream = req.value(ollama::kStream, true);

            auto openai_req = convert_ollama_to_openai_completion(req);

            if (stream) {
                LOG(INFO, "OllamaApi") << "POST /api/generate - Streaming (model: " << model << ")" << std::endl;

                openai_req[common::kStream.name] = true;
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
            write_plain_error(res, ApiRoute::kUnexpectedException, e.what());
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_ollama_generate_route(ServerContext& ctx) {
    return std::make_unique<GenerateRoute>(ctx);
}

} // namespace lemon
