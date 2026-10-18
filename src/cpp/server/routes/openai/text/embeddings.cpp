#include "lemon/router.h"
#include "lemon/server/api_route.h"
#include "lemon/server/model_loader.h"
#include "lemon/server/openai_args.h"
#include "lemon/server/shared_args.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

const RouteError kCannotEmbed{400, "", "`model`'s backend cannot embed, such as a text-to-speech model"};
const RouteError kNotEmbeddingModel{501, "", "`model` is a llama.cpp model that is not an embedding model"};

} // namespace

namespace openai::embeddings {

const RouteArg kInput{"input", ArgIn::JsonBody, {{"type", json::array({"string", "array"})}}, true, Support::Available,
                      "The text to embed, as a string or an array of strings.", {}, true};
const RouteArg kEncodingFormat{"encoding_format", ArgIn::JsonBody, {{"enum", json::array({"float", "base64"})}}, false, Support::Available,
                               "`float` or `base64`. Defaults to `float`.", {}, true};

} // namespace openai::embeddings

namespace {

using namespace openai::embeddings;

class EmbeddingsRoute : public ModelRoute {
public:
    using ModelRoute::ModelRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "openai.embeddings";
        s.methods = {"POST"};
        s.paths = {"embeddings"};
        s.summary = "Embed text as vectors";
        s.modality = "text -> vector representations";
        s.description =
            "Returns vector representations (embeddings) of the input text for semantic search, "
            "clustering and similarity comparisons, downloading and loading the model if it is not "
            "loaded. Embedding models use the `llamacpp` or `flm` recipe.";
        s.args = {kInput, model_loading::kModel, kEncodingFormat, model_loading::kCtxSize};

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = ordered_json::parse(R"({
            "type": "object",
            "required": ["object", "data", "model"],
            "properties": {
                "object": {"const": "list", "description": "Always `\"list\"`."},
                "data": {"type": "array", "description": "One embedding per input.", "items": {
                    "type": "object",
                    "required": ["object", "index", "embedding"],
                    "properties": {
                        "object": {"const": "embedding", "description": "Always `\"embedding\"`."},
                        "index": {"type": "integer", "description": "Position of the input text in the request."},
                        "embedding": {"type": ["array", "string"], "description": "The vector, as an array of floats, or a base64 string when `encoding_format` is `base64`."}
                    }
                }},
                "model": {"type": "string", "description": "The model that produced the embeddings."},
                "usage": {
                    "type": "object",
                    "description": "Token usage.",
                    "properties": {
                        "prompt_tokens": {"type": "integer", "description": "Tokens in the input."},
                        "total_tokens": {"type": "integer", "description": "Tokens processed in total."}
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
        s.errors = join_errors({{kBodyNotJson, kArgMissing, kArgInvalid, kCannotEmbed,
                                 kNotEmbeddingModel, kBackendError, kUnexpectedException},
                                ModelLoader::errors()});
        s.error_format = ErrorFormat::OpenAi;
        s.request_format = RequestFormat::Json;
        return s;
    }

protected:
    const RouteArg& model_arg() const override { return model_loading::kModel; }

    void run(RouteRequest& req, httplib::Response& res) override {
        auto response = ctx_.router->embeddings(req.body);
        if (response.contains("error")) {
            set_error_response(response, res, kBackendError);
            return;
        }
        res.set_content(response.dump(), "application/json");
    }

    LoadSpan load_span() const override { return {"EMBEDDING", "embeddings"}; }
};

} // namespace

std::unique_ptr<ApiRoute> make_embeddings_route(ServerContext& ctx) {
    return std::make_unique<EmbeddingsRoute>(ctx);
}

} // namespace lemon
