#include "lemon/router.h"
#include "lemon/server/api_route.h"

namespace lemon {
namespace {

using json = nlohmann::json;

class EmbeddingsRoute : public ModelRoute {
public:
    using ModelRoute::ModelRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "openai.embeddings";
        s.methods = {"POST"};
        s.paths = {"embeddings"};
        s.summary = "Embeddings";
        s.description =
            "Returns vector representations of input text for semantic search, clustering and "
            "similarity comparisons, loading the model on first use.";
        s.notes = {
            "Only embedding models (the `embeddings` label) using the `llamacpp` or `flm` recipe "
            "serve this endpoint.",
        };
        s.args = {
            {"model", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
             "Embedding model to run; loaded on first use."},
            {"input", ArgIn::JsonBody, {{"type", json::array({"string", "array"})}}, true, Support::Available,
             "Text to embed, or an array of texts."},
            {"encoding_format", ArgIn::JsonBody, {{"enum", json::array({"float", "base64"})}}, false, Support::Available,
             "Format of the returned embeddings: `float` (default) or `base64`."},
            {"ctx_size", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
             "Lemonade extension: context size to load the model with, when this request "
             "loads it. Ignored when the model is already loaded."},
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = json::parse(R"({
            "type": "object",
            "required": ["object", "data", "model"],
            "properties": {
                "object": {"const": "list"},
                "data": {"type": "array", "items": {
                    "type": "object",
                    "required": ["object", "index", "embedding"],
                    "properties": {
                        "object": {"const": "embedding"},
                        "index": {"type": "integer", "description": "Position of the input text in the request."},
                        "embedding": {"type": ["array", "string"], "description": "The vector, as floats or a base64 string."}
                    }
                }},
                "model": {"type": "string"},
                "usage": {
                    "type": "object",
                    "properties": {
                        "prompt_tokens": {"type": "integer"},
                        "total_tokens": {"type": "integer"}
                    }
                }
            }
        })");
        response.example = json::parse(R"({
            "model": "nomic-embed-text-v1-GGUF",
            "input": ["Hello, world!", "How are you?"],
            "encoding_format": "float"
        })");
        s.responses = {response};
        s.request_format = RequestFormat::Json;
        s.model_defaults_to_loaded = true;
        return s;
    }

protected:
    void run(RouteRequest& req, httplib::Response& res) override {
        auto response = ctx_.router->embeddings(req.body);
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

    LoadSpan load_span() const override { return {"EMBEDDING", "embeddings"}; }
};

} // namespace

std::unique_ptr<ApiRoute> make_embeddings_route(ServerContext& ctx) {
    return std::make_unique<EmbeddingsRoute>(ctx);
}

} // namespace lemon
