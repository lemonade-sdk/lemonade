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

json health_schema() {
    return json::parse(R"({
        "type": "object",
        "required": ["status", "version", "model_loaded", "all_models_loaded", "max_models",
                     "pinned_models", "pinned_helper_models", "update_check_done"],
        "properties": {
            "status": {"const": "ok"},
            "version": {"type": "string", "description": "Lemonade Server version."},
            "telemetry": {
                "type": "object",
                "required": ["enabled"],
                "properties": {
                    "enabled": {"type": "boolean", "description": "Whether telemetry collection is active."},
                    "captures": {"type": "array", "items": {"enum": ["inputs", "outputs", "thinking"]},
                                 "description": "Only when enabled: the request parts telemetry records."}
                }
            },
            "model_loaded": {"type": ["string", "null"], "description": "The most recently used loaded model, or null."},
            "all_models_loaded": {
                "type": "array",
                "description": "Every loaded model whose backend is alive.",
                "items": {
                    "type": "object",
                    "required": ["model_name", "checkpoint", "type", "device", "recipe", "recipe_options",
                                 "pinned", "pid", "backend_url", "last_use"],
                    "properties": {
                        "model_name": {"type": "string"},
                        "checkpoint": {"type": "string"},
                        "type": {"type": "string", "description": "Model type, such as llm, embedding, reranking, transcription, image, tts or classification."},
                        "residency_class": {"type": "string", "description": "standard, or routing_helper for a model a router policy keeps resident."},
                        "slot_pool": {"type": "string", "description": "The loaded-model limit the model counts against, or unmetered."},
                        "device": {"type": "string", "description": "Space-separated devices: cpu, gpu, npu, or a combination such as \"gpu npu\"."},
                        "backend_url": {"type": "string", "description": "URL of the backend process serving the model, for debugging."},
                        "pid": {"type": "integer", "description": "Process id of the backend."},
                        "launch_command": {"type": "array", "items": {"type": "string"},
                                           "description": "The program and arguments that started the backend, with the values actually used: an automatic ctx_size appears as a number, and flags Lemonade added are included. Absent for cloud models, which start no program."},
                        "status": {"type": "string"},
                        "backend_alive": {"type": "boolean"},
                        "backend_health": {"type": "string"},
                        "loaded": {"type": "boolean"},
                        "watchdog_reset": {"type": "boolean", "description": "Whether the backend watchdog has reset this backend."},
                        "watchdog_reset_reason": {"type": "string"},
                        "pinned": {"type": "boolean", "description": "Whether the model is pinned against eviction."},
                        "recipe": {"type": "string"},
                        "recipe_options": {"type": "object", "description": "Options the model was loaded with, such as ctx_size or llamacpp_backend."},
                        "is_busy": {"type": "boolean", "description": "Whether the model has requests or maintenance in progress."},
                        "is_streaming": {"type": "boolean", "description": "Whether the model is generating output: true from a stream's first chunk until every stream completes."},
                        "max_context_window": {"type": "integer"},
                        "cost_input_per_million": {"type": "number"},
                        "cost_output_per_million": {"type": "number"},
                        "last_use": {"type": "integer", "description": "Time of the last load or inference, in milliseconds on the server's monotonic clock: compare values, do not read them as dates."}
                    }
                }
            },
            "max_models": {"type": "object", "additionalProperties": {"type": "integer"},
                           "description": "Most models of each type that can be loaded at once, set by max_loaded_models."},
            "pinned_models": {"type": "object", "additionalProperties": {"type": "integer"},
                              "description": "Pinned loaded models of each type."},
            "pinned_helper_models": {"type": "object", "additionalProperties": {"type": "integer"},
                                     "description": "Pinned routing-helper models of each type."},
            "websocket_port": {"type": "integer",
                               "description": "Only while the WebSocket server runs: the dedicated port of the Realtime and Log Streaming APIs, OS-assigned or set by --websocket-port. The main port serves both APIs too."},
            "update_check_done": {"type": "boolean",
                                  "description": "Whether the startup model update check has finished; update_available fields are ready once it has."}
        }
    })");
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
        s.description = "Reports that the server is up, its version, and the models it has loaded.";
        s.notes = {
            "`HEAD` returns `200 OK` with an empty body.",
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = health_schema();
        response.setup = {{"openai.chat_completions", ResponseFormat::Json}};
        response.example = json::object();
        s.responses = {response};
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
