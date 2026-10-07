#include <lemon/utils/aixlog.hpp>

#include "lemon/error_types.h"
#include "lemon/router.h"
#include "lemon/server/api_route.h"
#include "lemon/server/gateway_conversion.h"
#include "lemon/server/model_loader.h"

namespace lemon {
namespace {

using json = nlohmann::json;

class EmbeddingsRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "ollama.embeddings";
        s.methods = {"POST"};
        s.paths = {"/api/embeddings"};
        s.prefixes = Prefixes::Root;
        s.summary = "Legacy embeddings";
        s.description =
            "Returns the embedding of one text in Ollama's older format, loading the model on "
            "first use.";
        s.notes = {
            "Only the first input's embedding is returned. An unknown model answers `404` with "
            "`model '<name>' not found`.",
        };
        s.args = {
            {"model", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
             "Embedding model to run; loaded on first use. A `:latest` tag is ignored."},
            {"prompt", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
             "Text to embed."},
            {"input", ArgIn::JsonBody, {{"type", json::array({"string", "array"})}}, false, Support::Available,
             "Accepted in place of `prompt`."},
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
            "required": ["model", "embedding"],
            "properties": {
                "model": {"type": "string"},
                "embedding": {"type": "array", "items": {"type": "number"}}
            }
        })");
        response.example = json::parse(R"({
            "model": "nomic-embed-text-v1-GGUF",
            "prompt": "Why is the sky blue?"
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

            if (request_json.contains("prompt")) {
                openai_req["input"] = request_json["prompt"];
            } else if (request_json.contains("input")) {
                openai_req["input"] = request_json["input"];
            } else {
                res.status = 400;
                res.set_content(R"({"error":"prompt is required"})", "application/json");
                return;
            }

            auto openai_response = ctx_.router->embeddings(openai_req);

            if (write_ollama_backend_error(openai_response, res)) return;

            json ollama_res;
            ollama_res["model"] = model;

            if (openai_response.contains("data") && openai_response["data"].is_array() &&
                !openai_response["data"].empty()) {
                ollama_res["embedding"] = openai_response["data"][0]["embedding"];
            } else {
                ollama_res["embedding"] = json::array();
            }

            res.set_content(ollama_res.dump(), "application/json");

        } catch (const std::exception& e) {
            LOG(ERROR, "OllamaApi") << "Error in /api/embeddings: " << e.what() << std::endl;
            res.status = 500;
            json error = {{"error", std::string(e.what())}};
            res.set_content(error.dump(), "application/json");
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_ollama_embeddings_route(ServerContext& ctx) {
    return std::make_unique<EmbeddingsRoute>(ctx);
}

} // namespace lemon
