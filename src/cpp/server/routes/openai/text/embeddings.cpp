#include "lemon/router.h"
#include "lemon/server/api_route.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

class EmbeddingsRoute : public ModelRoute {
public:
    using ModelRoute::ModelRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "openai.embeddings";
        s.methods = {"POST"};
        s.paths = {"embeddings"};
        s.summary = "Embeddings";
        s.modality = "text -> vector representations";
        s.description =
            "Returns vector representations (embeddings) of the input text for semantic search, "
            "clustering and similarity comparisons, loading the model on first use. Embedding "
            "models use the `llamacpp` or `flm` recipe.";
        s.args = {
            {"input", ArgIn::JsonBody, {{"type", json::array({"string", "array"})}}, true, Support::Available,
             "The text to embed, as a string or an array of strings."},
            {"model", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
             "The model to use for generating embeddings."},
            {"encoding_format", ArgIn::JsonBody, {{"enum", json::array({"float", "base64"})}}, false, Support::Available,
             "The format to return embeddings in. Supported values: `\"float\"` (default), "
             "`\"base64\"`."},
            {"ctx_size", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
             "Lemonade extension: context size to load the model with, when this request "
             "loads it. Ignored when the model is already loaded."},
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = ordered_json::parse(R"({
            "type": "object",
            "required": ["object", "data", "model"],
            "properties": {
                "object": {"const": "list", "description": "Type of response object, always `\"list\"`"},
                "data": {"type": "array", "description": "Array of embedding objects", "items": {
                    "type": "object",
                    "required": ["object", "index", "embedding"],
                    "properties": {
                        "object": {"const": "embedding", "description": "Type of embedding object, always `\"embedding\"`"},
                        "index": {"type": "integer", "description": "Index position of the input text in the request"},
                        "embedding": {"type": ["array", "string"], "description": "Vector representation as an array of floats"}
                    }
                }},
                "model": {"type": "string", "description": "Model identifier used to generate the embeddings"},
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
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.args = ordered_json::parse(R"({
            "model": "nomic-embed-text-v1-GGUF",
            "input": ["Hello, world!", "How are you?"],
            "encoding_format": "float"
        })");
        s.examples = {example};
        s.request_format = RequestFormat::Json;
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
