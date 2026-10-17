#include "lemon/router.h"
#include "lemon/server/api_route.h"
#include "lemon/server/model_loader.h"
#include "lemon/server/shared_args.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

const RouteError kNotLlamaCpp{400, "", "`model` is not a llama.cpp model"};

const RouteArg kQuery{"query", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
                      "The search query text.", {}, true};
const RouteArg kDocuments{"documents", ArgIn::JsonBody, {{"type", "array"}, {"items", {{"type", "string"}}}},
                          true, Support::Available, "Documents to score against the query.", {}, true};

class RerankRoute : public ModelRoute {
public:
    using ModelRoute::ModelRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "llamacpp.rerank";
        s.methods = {"POST"};
        s.paths = {"rerank", "reranking", "reranker"};
        s.summary = "Score documents against a query";
        s.modality = "query + documents -> relevance-scored documents";
        s.description =
            "Scores each document against a query. Only llama.cpp reranker models, such as "
            "`bge-reranker-v2-m3-GGUF`, serve this endpoint.";
        s.args = {kQuery, kDocuments, model_loading::kModel, model_loading::kCtxSize};

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = ordered_json::parse(R"({
            "type": "object",
            "required": ["model", "object", "results"],
            "properties": {
                "model": {"type": "string", "description": "The model that scored the documents."},
                "object": {"const": "list", "description": "Always `\"list\"`."},
                "results": {"type": "array", "description": "Every input document with its score, sorted by `relevance_score`, highest first.", "items": {
                    "type": "object",
                    "required": ["index", "relevance_score"],
                    "properties": {
                        "index": {"type": "integer", "description": "The document's position in `documents`."},
                        "relevance_score": {"type": "number", "description": "How relevant the model rates the document; higher is more relevant."}
                    }
                }},
                "usage": {
                    "type": "object",
                    "description": "Tokens processed.",
                    "properties": {
                        "prompt_tokens": {"type": "integer", "description": "Tokens in the query and documents."},
                        "total_tokens": {"type": "integer", "description": "All tokens processed."}
                    }
                }
            }
        })");
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.args = ordered_json::parse(R"({
            "model": "bge-reranker-v2-m3-GGUF",
            "query": "What is the capital of France?",
            "documents": [
                "Paris is the capital of France.",
                "Berlin is the capital of Germany.",
                "Madrid is the capital of Spain."
            ]
        })");
        s.examples = {example};
        s.errors = join_errors({{kBodyNotJson, kArgMissing, kArgInvalid, kNotLlamaCpp,
                                 kBackendError, kUnexpectedException},
                                ModelLoader::errors()});
        s.error_format = ErrorFormat::OpenAi;
        s.request_format = RequestFormat::Json;
        return s;
    }

protected:
    const RouteArg& model_arg() const override { return model_loading::kModel; }

    void run(RouteRequest& req, httplib::Response& res) override {
        auto response = ctx_.router->reranking(req.body);
        if (response.contains("error")) {
            set_error_response(response, res, kBackendError);
            return;
        }
        res.set_content(response.dump(), "application/json");
    }

    LoadSpan load_span() const override { return {"RERANKER", "reranking"}; }
};

} // namespace

std::unique_ptr<ApiRoute> make_rerank_route(ServerContext& ctx) {
    return std::make_unique<RerankRoute>(ctx);
}

} // namespace lemon
