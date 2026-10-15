#include <lemon/utils/aixlog.hpp>

#include "lemon/router.h"
#include "lemon/server/api_route.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

const RouteError kInvalidJson{400, "", "The body is not empty and is not valid JSON"};
const RouteError kMissingContent{400, "", "`content` is missing or not a string"};
const RouteError kNoModelLoaded{400, "", "No model is loaded"};
const RouteError kModelUnloaded{404, "", "The model is unloaded while the request waits for it"};
const RouteError kNotLlamaCpp{400, "", "The most recently used model is not a llama.cpp model"};
const RouteError kTokenizeFailed{500, "", "Tokenizing fails unexpectedly"};

class TokenizeRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "llamacpp.tokenize";
        s.methods = {"POST"};
        s.paths = {"tokenize"};
        s.summary = "Tokenize text";
        s.modality = "tokenization";
        s.description =
            "Tokenizes text without using the model's context window. After a request to a "
            "reranker, for example, the reranker's tokenizer answers.";
        s.args = {
            {"content", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
             "The text to tokenize."},
            {"add_special", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
             "Insert special tokens, such as `BOS`. Defaults to `false`."},
            {"parse_special", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
             "Tokenize special tokens; `false` treats them as plain text. Defaults to `true`."},
            {"with_pieces", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
             "Return each token's text piece with its ID. Defaults to `false`."},
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = ordered_json::parse(R"({
            "type": "object",
            "required": ["tokens"],
            "properties": {
                "tokens": {
                    "type": "array",
                    "description": "Token IDs, or `{\"id\", \"piece\"}` objects when `with_pieces` is `true`.",
                    "items": {"type": ["integer", "object"]}
                }
            }
        })");
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.setup = {{"lemonade.load", "model"}};
        example.args = {{"content", "This is a string to tokenize"}};
        s.examples = {example};
        s.errors = {kInvalidJson, kMissingContent, kNoModelLoaded, kModelUnloaded, kNotLlamaCpp,
                    kBackendError, kTokenizeFailed};
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
                    write_plain_error(res, kInvalidJson, "Invalid JSON in request body");
                    return;
                }
            }

            if (!request_body.contains("content") || !request_body["content"].is_string()) {
                LOG(ERROR, "Server") << "Tokenization failed: 'content' parameter is missing" << std::endl;
                write_plain_error(res, kMissingContent, "'content' parameter is required");
                return;
            }

            if (!ctx_.router->is_model_loaded()) {
                LOG(ERROR, "Server") << "No model loaded for tokenization" << std::endl;
                write_plain_error(res, kNoModelLoaded, "No model loaded for tokenization");
                return;
            }

            auto response = ctx_.router->tokenize(request_body);
            if (response.contains("error")) {
                set_error_response(response, res, kBackendError);
                return;
            }

            res.set_content(response.dump(), "application/json");
        } catch (const std::exception& e) {
            LOG(ERROR, "Server") << "ERROR in handle_tokenize: " << e.what() << std::endl;
            write_plain_error(res, kTokenizeFailed, e.what());
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_tokenize_route(ServerContext& ctx) {
    return std::make_unique<TokenizeRoute>(ctx);
}

} // namespace lemon
