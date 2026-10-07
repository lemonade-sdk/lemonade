#include "lemon/router.h"
#include "lemon/server/api_route.h"

namespace lemon {
namespace {

using json = nlohmann::json;

class RerankRoute : public ModelRoute {
public:
    using ModelRoute::ModelRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "llamacpp.rerank";
        s.methods = {"POST"};
        s.paths = {"rerank", "reranking", "reranker"};
        s.summary = "Score documents by relevance to a query";
        s.description =
            "Scores each document by its relevance to a query, loading the reranking model on "
            "first use.";
        s.notes = {
            "Lemonade forwards the request to llama.cpp's `/v1/rerank`, so only reranking models "
            "(the `reranking` label) using the `llamacpp` recipe, such as "
            "`bge-reranker-v2-m3-GGUF`, serve it. `/rerank` is the path most clients expect; "
            "`/reranking` and `/reranker` behave identically.",
            "Results are returned in input order. To rank documents, sort `results` by "
            "`relevance_score`, highest first.",
        };
        s.args = {
            {"model", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
             "Reranking model to run; loaded on first use."},
            {"query", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
             "Search query the documents are scored against."},
            {"documents", ArgIn::JsonBody, {{"type", "array"}, {"items", {{"type", "string"}}}},
             true, Support::Available, "Document strings to score against the query."},
            {"ctx_size", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
             "Lemonade extension: context size to load the model with, when this request "
             "loads it. Ignored when the model is already loaded."},
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = json::parse(R"({
            "type": "object",
            "required": ["model", "object", "results"],
            "properties": {
                "model": {"type": "string", "description": "Model that scored the documents."},
                "object": {"const": "list"},
                "results": {"type": "array", "description": "One entry per input document, in input order.", "items": {
                    "type": "object",
                    "required": ["index", "relevance_score"],
                    "properties": {
                        "index": {"type": "integer", "description": "Position of the document in the request."},
                        "relevance_score": {"type": "number", "description": "Relevance to the query; higher is more relevant."}
                    }
                }},
                "usage": {
                    "type": "object",
                    "properties": {
                        "prompt_tokens": {"type": "integer", "description": "Tokens in the input."},
                        "total_tokens": {"type": "integer", "description": "Tokens processed."}
                    }
                }
            }
        })");
        response.example = json::parse(R"({
            "model": "jina-reranker-v1-tiny-en-GGUF",
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
