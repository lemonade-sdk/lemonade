#include <lemon/utils/aixlog.hpp>

#include "lemon/router.h"
#include "lemon/server/api_route.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

class TokenizeRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "llamacpp.tokenize";
        s.methods = {"POST"};
        s.paths = {"tokenize"};
        s.summary = "Tokenize a given text";
        s.modality = "tokenization";
        s.description =
            "Tokenize a given text. Does not count towards the current model's context window.\n\n"
            "> **Note:** This endpoint is part of Lemonade's llama.cpp compatibility layer. "
            "Internally, Lemonade forwards the request to llama.cpp's `/tokenize` endpoint.\n\n"
            "> **Note:** This endpoint supports all four path prefixes: `/api/v0/tokenize`, "
            "`/api/v1/tokenize`, `/v0/tokenize`, and `/v1/tokenize`.\n\n"
            "> **Note:** Actual response values may vary for the same string across different "
            "models if the models do not share the same tokenizer.";
        s.notes = {
            "The request names no model: Lemonade forwards it to llama.cpp's `/tokenize` on the most "
            "recently used loaded model, which must be a llama.cpp model, so after a request to a "
            "reranker, for example, the reranker's tokenizer answers. With no model loaded, or "
            "when that model is not a llama.cpp model, the answer is `400`.",
        };
        s.args = {
            {"content", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
             "The text to tokenize."},
            {"add_special", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
             "Boolean indicating if special tokens, i.e. `BOS`, should be inserted. Default: "
             "`false`"},
            {"parse_special", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
             "Boolean indicating if special tokens should be tokenized. When `false` special "
             "tokens are treated as plaintext. Default: `true`"},
            {"with_pieces", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
             "Boolean indicating whether to return token pieces along with IDs. Default: "
             "`false`"},
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = ordered_json::parse(R"({
            "type": "object",
            "required": ["tokens"],
            "properties": {
                "tokens": {
                    "type": "array",
                    "description": "Array of token IDs, or of `{\"id\", \"piece\"}` objects when `with_pieces` is `true`",
                    "items": {"type": ["integer", "object"]}
                }
            }
        })");
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.setup = {{"lemonade.load", ""}};
        example.args = {{"content", "This is a string to tokenize"}};
        s.examples = {example};
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        try {
            LOG(INFO, "Server") << "POST /api/v1/tokenize" << std::endl;

            json request_body = json::object();
            if (!req.http.body.empty()) {
                try {
                    request_body = json::parse(req.http.body);
                } catch (const std::exception& e) {
                    LOG(ERROR, "Server") << "Failed to parse request body: " << e.what() << std::endl;
                    write_plain_error(res, 400, "Invalid JSON in request body");
                    return;
                }
            }

            if (!request_body.contains("content") || !request_body["content"].is_string()) {
                LOG(ERROR, "Server") << "Tokenization failed: 'content' parameter is missing" << std::endl;
                write_plain_error(res, 400, "'content' parameter is required");
                return;
            }

            if (!ctx_.router->is_model_loaded()) {
                LOG(ERROR, "Server") << "No model loaded for tokenization" << std::endl;
                write_plain_error(res, 400, "No model loaded for tokenization");
                return;
            }

            auto response = ctx_.router->tokenize(request_body);
            if (response.contains("error")) {
                set_error_response(response, res);
                return;
            }

            res.set_content(response.dump(), "application/json");
        } catch (const std::exception& e) {
            LOG(ERROR, "Server") << "ERROR in handle_tokenize: " << e.what() << std::endl;
            write_plain_error(res, 500, e.what());
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_tokenize_route(ServerContext& ctx) {
    return std::make_unique<TokenizeRoute>(ctx);
}

} // namespace lemon
