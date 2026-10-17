#include <lemon/utils/aixlog.hpp>

#include "lemon/router.h"
#include "lemon/server/api_route.h"
#include "lemon/server/gateway_conversion.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;


class EmbeddingsRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "ollama.embeddings";
        s.methods = {"POST"};
        s.paths = {"/api/embeddings"};
        s.prefixes = Prefixes::Root;
        s.summary = "Embed one text in the older format";
        s.description =
            "Returns the embedding of one text in Ollama's older format, loading the model on "
            "first use.";
        s.args = {
            {"model", ArgIn::JsonBody, {{"type", "string"}, {"minLength", 1}}, true, Support::Available,
             "Embedding model to run."},
            {"prompt", ArgIn::JsonBody, {{"type", json::array({"string", "array"})}}, true, Support::Available,
             "Text to embed. For an array, only the first text's embedding is returned.",
             {"input"}},
            {"options", ArgIn::JsonBody, {{"type", "object"}}, false, Support::Partial,
             "`num_ctx` sets the context size when this request loads the model. Other keys are "
             "ignored."},
            {"ctx_size", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
             "Lemonade extension: context size to load the model with, when this request loads "
             "it. Wins over `options.num_ctx`. Ignored unless an integer.", {}, false, true},
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = ordered_json::parse(R"({
            "type": "object",
            "required": ["model", "embedding"],
            "properties": {
                "model": {"type": "string", "description": "The model that produced the embedding."},
                "embedding": {"type": "array", "items": {"type": "number"}, "description": "The text's embedding vector."}
            }
        })");
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.args = ordered_json::parse(R"({
            "model": "nomic-embed-text-v1-GGUF",
            "prompt": "Why is the sky blue?"
        })");
        s.examples = {example};
        s.errors = join_errors({{kBodyNotJson, kArgMissing, kArgInvalid}, gateway_load_errors(),
                                {kOllamaBackendError,
                                 ApiRoute::kUnexpectedException}});
        s.request_format = RequestFormat::Json;
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        try {
            const json& request_json = req.body;

            std::string model = strip_latest_tag(request_json["model"].get<std::string>());

            if (!gateway_ensure_loaded(*ctx_.model_loader, model, request_json, ErrorFormat::Plain,
                                       res)) {
                return;
            }

            json openai_req;
            openai_req["model"] = model;

            openai_req["input"] = request_json["prompt"];

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
            write_plain_error(res, ApiRoute::kUnexpectedException, e.what());
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_ollama_embeddings_route(ServerContext& ctx) {
    return std::make_unique<EmbeddingsRoute>(ctx);
}

} // namespace lemon
