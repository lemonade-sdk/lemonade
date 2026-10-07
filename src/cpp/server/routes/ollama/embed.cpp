#include <lemon/utils/aixlog.hpp>

#include "lemon/error_types.h"
#include "lemon/router.h"
#include "lemon/server/api_route.h"
#include "lemon/server/gateway_conversion.h"
#include "lemon/server/model_loader.h"

namespace lemon {
namespace {

using json = nlohmann::json;

class EmbedRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "ollama.embed";
        s.methods = {"POST"};
        s.paths = {"/api/embed"};
        s.prefixes = Prefixes::Root;
        s.summary = "Embeddings";
        s.description =
            "Returns one embedding per input text, loading the model on first use.";
        s.notes = {
            "An unknown model answers `404` with `model '<name>' not found`.",
        };
        s.args = {
            {"model", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
             "Embedding model to run; loaded on first use. A `:latest` tag is ignored."},
            {"input", ArgIn::JsonBody, {{"type", json::array({"string", "array"})}}, true, Support::Available,
             "Text to embed, or an array of texts."},
            {"options", ArgIn::JsonBody, {{"type", "object"}}, false, Support::Available,
             "`num_ctx` sets the context size when this request loads the model."},
            {"ctx_size", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
             "Lemonade extension: context size to load the model with, when this request loads "
             "it. Wins over `options.num_ctx`."},
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = json::parse(R"({
            "type": "object",
            "required": ["model", "embeddings"],
            "properties": {
                "model": {"type": "string"},
                "embeddings": {"type": "array", "items": {"type": "array", "items": {"type": "number"}},
                               "description": "One vector per input, in input order."},
                "total_duration": {"type": "integer", "description": "Always 0."},
                "load_duration": {"type": "integer", "description": "Always 0."},
                "prompt_eval_count": {"type": "integer", "description": "Always 0."}
            }
        })");
        response.example = json::parse(R"({
            "model": "nomic-embed-text-v1-GGUF",
            "input": "Why is the sky blue?"
        })");
        s.responses = {response};
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

            try {
                ctx_.model_loader->ensure_loaded(model,
                                                 gateway_load_options(request_json));
            } catch (const RouterResidencyConflictException& e) {
                write_ollama_residency_conflict(e, res);
                return;
            } catch (const std::exception& e) {
                res.status = 404;
                json error = {{"error", "model '" + model + "' not found"}};
                res.set_content(error.dump(), "application/json");
                return;
            }

            json openai_req;
            openai_req["model"] = model;

            if (request_json.contains("input")) {
                openai_req["input"] = request_json["input"];
            } else {
                res.status = 400;
                res.set_content(R"({"error":"input is required"})", "application/json");
                return;
            }

            auto openai_response = ctx_.router->embeddings(openai_req);

            if (write_ollama_backend_error(openai_response, res)) return;

            json ollama_res;
            ollama_res["model"] = model;
            ollama_res["embeddings"] = json::array();

            if (openai_response.contains("data") && openai_response["data"].is_array()) {
                for (const auto& item : openai_response["data"]) {
                    if (item.contains("embedding")) {
                        ollama_res["embeddings"].push_back(item["embedding"]);
                    }
                }
            }

            ollama_res["total_duration"] = 0;
            ollama_res["load_duration"] = 0;
            ollama_res["prompt_eval_count"] = 0;

            res.set_content(ollama_res.dump(), "application/json");

        } catch (const std::exception& e) {
            LOG(ERROR, "OllamaApi") << "Error in /api/embed: " << e.what() << std::endl;
            res.status = 500;
            json error = {{"error", std::string(e.what())}};
            res.set_content(error.dump(), "application/json");
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_ollama_embed_route(ServerContext& ctx) {
    return std::make_unique<EmbedRoute>(ctx);
}

} // namespace lemon
