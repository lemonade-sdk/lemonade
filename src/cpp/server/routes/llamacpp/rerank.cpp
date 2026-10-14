#include "lemon/router.h"
#include "lemon/server/api_route.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

class RerankRoute : public ModelRoute {
public:
    using ModelRoute::ModelRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "llamacpp.rerank";
        s.methods = {"POST"};
        s.paths = {"rerank", "reranking", "reranker"};
        s.summary = "Reranking";
        s.modality = "query + documents -> relevance-scored documents";
        s.description =
            "Reranking API for llama.cpp-compatible reranker models. You provide a query and a "
            "list of documents, and receive relevance scores for each document. Lemonade will load "
            "the requested model automatically if it is not already loaded.\n\n"
            "> **Note:** This endpoint is part of Lemonade's llama.cpp compatibility layer. "
            "Internally, Lemonade forwards the request to llama.cpp's `/v1/rerank` endpoint.\n\n"
            "> **Note:** Lemonade also accepts `/reranking` and `/reranker` as aliases — all "
            "three route to the same handler and behave identically.\n\n"
            "> **Note:** The endpoint is available under all four path prefixes: `/api/v0/`, "
            "`/api/v1/`, `/v0/`, and `/v1/`.\n\n"
            "> **Note:** This endpoint is only available for reranker-specific models using the "
            "`llamacpp` recipe, such as `bge-reranker-v2-m3-GGUF`.";
        s.args = {
            {"query", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
             "The search query text."},
            {"documents", ArgIn::JsonBody, {{"type", "array"}, {"items", {{"type", "string"}}}},
             true, Support::Available, "Array of document strings to score against the query."},
            {"model", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
             "The reranking model to use. If not already loaded, Lemonade loads it before "
             "forwarding the request."},
            {"ctx_size", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
             "Lemonade extension: context size to load the model with, when this request "
             "loads it. Ignored when the model is already loaded."},
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = ordered_json::parse(R"({
            "type": "object",
            "required": ["model", "object", "results"],
            "properties": {
                "model": {"type": "string", "description": "Model identifier used for reranking"},
                "object": {"const": "list", "description": "Type of response object, always `\"list\"`"},
                "results": {"type": "array", "description": "Array of all input documents with relevance scores, sorted by `relevance_score`, highest first", "items": {
                    "type": "object",
                    "required": ["index", "relevance_score"],
                    "properties": {
                        "index": {"type": "integer", "description": "Original index of the document in the input array"},
                        "relevance_score": {"type": "number", "description": "Relevance score assigned by the model; higher means more relevant"}
                    }
                }},
                "usage": {
                    "type": "object",
                    "description": "Token usage statistics",
                    "properties": {
                        "prompt_tokens": {"type": "integer", "description": "Number of tokens in the input"},
                        "total_tokens": {"type": "integer", "description": "Total tokens processed"}
                    }
                }
            }
        })");
        response.example = ordered_json::parse(R"({
            "model": "bge-reranker-v2-m3-GGUF",
            "query": "What is the capital of France?",
            "documents": [
                "Paris is the capital of France.",
                "Berlin is the capital of Germany.",
                "Madrid is the capital of Spain."
            ]
        })");
        s.responses = {response};
        s.request_format = RequestFormat::Json;
        return s;
    }

protected:
    void run(RouteRequest& req, httplib::Response& res) override {
        auto response = ctx_.router->reranking(req.body);
        if (response.contains("error")) {
            set_error_response(response, res);
            return;
        }
        res.set_content(response.dump(), "application/json");
    }

    void write_invalid_body(const RouteRequest&, const std::exception& error,
                            httplib::Response& res) const override {
        write_plain_error(res, 500, error.what());
    }

    LoadSpan load_span() const override { return {"RERANKER", "reranking"}; }
};

} // namespace

std::unique_ptr<ApiRoute> make_rerank_route(ServerContext& ctx) {
    return std::make_unique<RerankRoute>(ctx);
}

} // namespace lemon
