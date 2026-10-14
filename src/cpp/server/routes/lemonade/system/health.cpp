#include <string>
#include <vector>

#include "lemon/router.h"
#include "lemon/runtime_config.h"
#include "lemon/server.h"
#include "lemon/server/api_route.h"
#include "lemon/server/http_listener.h"
#include "lemon/version.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

ordered_json health_schema() {
    return ordered_json::parse(R"json({
        "type": "object",
        "required": ["status", "version", "model_loaded", "all_models_loaded", "max_models",
                     "pinned_models", "pinned_helper_models", "update_check_done"],
        "properties": {
            "status": {"const": "ok", "description": "Server health status, always `\"ok\"`"},
            "version": {"type": "string", "description": "Version number of Lemonade Server"},
            "model_loaded": {"type": ["string", "null"], "description": "Model name of the most recently accessed model"},
            "update_check_done": {"type": "boolean", "description": "Whether the background HuggingFace model update check has completed at startup. Poll this field after server start to know when `update_available` fields are ready."},
            "all_models_loaded": {
                "type": "array",
                "description": "Array of all currently loaded models with details:",
                "items": {
                    "type": "object",
                    "required": ["model_name", "checkpoint", "type", "device", "recipe",
                                 "recipe_options", "pinned", "pid", "backend_url", "last_use"],
                    "properties": {
                        "model_name": {"type": "string", "description": "Name of the loaded model"},
                        "checkpoint": {"type": "string", "description": "Full checkpoint identifier"},
                        "last_use": {"type": "integer", "description": "Monotonic timestamp of last access (load or inference), in milliseconds"},
                        "type": {"type": "string", "description": "Model type: `\"llm\"`, `\"embedding\"`, `\"reranking\"`, `\"transcription\"`, `\"image\"`, `\"tts\"`, `\"audio-generation\"`, `\"classification\"`, or `\"mesh\"`"},
                        "device": {"type": "string", "description": "Space-separated device list: `\"cpu\"`, `\"gpu\"`, `\"npu\"`, or combinations like `\"gpu npu\"`"},
                        "pinned": {"type": "boolean", "description": "Boolean indicating if the model is currently pinned to prevent auto-eviction"},
                        "is_busy": {"type": "boolean", "description": "Boolean indicating if the model has active requests or maintenance in progress"},
                        "is_streaming": {"type": "boolean", "description": "Boolean indicating if the model is actively generating output tokens (true after first chunk arrives, false when all streaming requests complete)"},
                        "backend_url": {"type": "string", "description": "URL of the backend server process handling this model (useful for debugging)"},
                        "pid": {"type": "integer", "description": "The Process ID (PID) of the backend engine handling this model"},
                        "launch_command": {"type": "array", "items": {"type": "string"}, "description": "*(optional)* The command used to start the backend engine, as an array with the program first and its arguments after it. Every local backend has one. Cloud models don't, because they don't start a program. The values shown are the ones actually used, so a `ctx_size` of `auto` appears here as a real number, and any flags Lemonade added on its own are included."},
                        "recipe": {"type": "string", "description": "Backend/device recipe used to load the model (e.g., `\"ryzenai-llm\"`, `\"llamacpp\"`, `\"flm\"`)"},
                        "recipe_options": {"type": "object", "description": "Options used to load the model (e.g., `\"ctx_size\"`, `\"llamacpp_backend\"`, `\"llamacpp_args\"`, `\"whispercpp_args\"`)"},
                        "residency_class": {"type": "string", "description": "standard, or routing_helper for a model a router policy keeps resident."},
                        "slot_pool": {"type": "string", "description": "The loaded-model limit the model counts against, or unmetered."},
                        "status": {"type": "string"},
                        "backend_alive": {"type": "boolean"},
                        "backend_health": {"type": "string"},
                        "loaded": {"type": "boolean"},
                        "watchdog_reset": {"type": "boolean", "description": "Whether the backend watchdog has reset this backend."},
                        "watchdog_reset_reason": {"type": "string"},
                        "max_context_window": {"type": "integer"},
                        "cost_input_per_million": {"type": "number"},
                        "cost_output_per_million": {"type": "number"}
                    }
                }
            },
            "pinned_models": {"type": "object", "additionalProperties": {"type": "integer"}, "description": "Counts of pinned models currently loaded in memory per model type (e.g., `llm`, `embedding`, etc.)"},
            "max_models": {
                "type": "object",
                "description": "Maximum number of models that can be loaded simultaneously per type (set via `max_loaded_models` in [Server Configuration](../guide/configuration/README.md)):",
                "properties": {
                    "llm": {"type": "integer", "description": "Maximum LLM/chat models"},
                    "embedding": {"type": "integer", "description": "Maximum embedding models"},
                    "reranking": {"type": "integer", "description": "Maximum reranking models"},
                    "transcription": {"type": "integer", "description": "Maximum speech-to-text models"},
                    "image": {"type": "integer", "description": "Maximum image models"},
                    "tts": {"type": "integer", "description": "Maximum text-to-speech models"}
                },
                "additionalProperties": {"type": "integer"}
            },
            "websocket_port": {"type": "integer", "description": "*(optional)* Port of the WebSocket server for the [Realtime Audio Transcription API](./openai.md#ws-realtime) and [Log Streaming API](#log-streaming-api-websocket). Only present when the WebSocket server is running. The port is OS-assigned or set via `--websocket-port`. The main port serves both APIs too."},
            "telemetry": {
                "type": "object",
                "description": "Structured telemetry state object:",
                "required": ["enabled"],
                "properties": {
                    "enabled": {"type": "boolean", "description": "Boolean indicating if telemetry collection is active"},
                    "captures": {"type": "array", "items": {"enum": ["inputs", "outputs", "thinking"]}, "description": "*(optional)* Array of captured telemetry components (e.g., `[\"inputs\", \"outputs\", \"thinking\"]`), only present when `enabled` is `true`."}
                }
            },
            "pinned_helper_models": {"type": "object", "additionalProperties": {"type": "integer"}, "description": "Pinned routing-helper models of each type."}
        }
    })json");
}

class HealthRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "lemonade.health";
        s.methods = {"GET"};
        s.paths = {"health"};
        s.summary = "Check server status, such as models loaded";
        s.description =
            "Check the health of the server. This endpoint returns information about loaded models.";
        s.notes = {
            "`HEAD` returns `200 OK` with an empty body.",
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = health_schema();
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.setup = {{"openai.chat_completions", "non-streaming"}};
        example.args = json::object();
        s.examples = {example};
        s.quiet_log = true;
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        if (req.http.method == "HEAD") {
            res.status = 200;
            return;
        }

        RuntimeConfig& config = *ctx_.config;
        json response = {{"status", "ok"}};
        response["version"] = LEMON_VERSION_STRING;

        json telemetry_info = {{"enabled", config.telemetry_enabled()}};
        if (config.telemetry_enabled()) {
            std::vector<std::string> captures;
            if (!config.telemetry_hide_inputs()) {
                captures.push_back("inputs");
            }
            if (!config.telemetry_hide_outputs()) {
                captures.push_back("outputs");
            }
            if (!config.telemetry_hide_thinking()) {
                captures.push_back("thinking");
            }
            telemetry_info["captures"] = captures;
        }
        response["telemetry"] = telemetry_info;

        std::string loaded_model = ctx_.router->get_loaded_model();
        response["model_loaded"] = loaded_model.empty() ? json(nullptr) : json(loaded_model);
        response["all_models_loaded"] = ctx_.router->get_all_loaded_models();
        response["max_models"] = ctx_.router->get_max_model_limits();
        response["pinned_models"] = ctx_.router->get_pinned_model_counts();
        response["pinned_helper_models"] = ctx_.router->get_pinned_helper_counts();

        const int websocket_port = ctx_.listener->websocket_port();
        if (websocket_port > 0) {
            response["websocket_port"] = websocket_port;
        }

        response["update_check_done"] = ctx_.server->update_check_done();

        res.set_content(response.dump(), "application/json");
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_health_route(ServerContext& ctx) {
    return std::make_unique<HealthRoute>(ctx);
}

} // namespace lemon
