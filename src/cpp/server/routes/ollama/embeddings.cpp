#include <lemon/utils/aixlog.hpp>

#include "lemon/router.h"
#include "lemon/server/api_route.h"
#include "lemon/server/gateway_conversion.h"
#include "lemon/server/openai_args.h"
#include "lemon/server/shared_args.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;


const RouteArg kPrompt{"prompt", ArgIn::JsonBody, {{"type", json::array({"string", "array"})}}, true, Support::Available,
                       "Text to embed. For an array, only the first text's embedding is returned.",
                       {"input"}};

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
        s.args = {gateway::kModel, kPrompt, ollama::embed::kOptions, model_loading::kCtxSize};

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
            std::string model = strip_latest_tag(req.arg(gateway::kModel).get<std::string>());

            if (!gateway_ensure_loaded(*ctx_.model_loader, model,
                                       gateway_load_options(req.arg(ollama::embed::kOptions), req.arg(model_loading::kCtxSize)),
                                       ErrorFormat::Plain, res)) {
                return;
            }

            json openai_req;
            openai_req[model_loading::kModel.name] = model;

            openai_req[openai::embeddings::kInput.name] = req.arg(kPrompt);

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
