#include <lemon/utils/aixlog.hpp>

#include "lemon/router.h"
#include "lemon/server/api_route.h"

namespace lemon {
namespace {

using json = nlohmann::json;

class TokenizeRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "llamacpp.tokenize";
        s.methods = {"POST"};
        s.paths = {"tokenize"};
        s.summary = "Tokenize text";
        s.description =
            "Tokenizes text with a loaded llama.cpp model's tokenizer, without using any of the "
            "model's context window.";
        s.notes = {
            "The request names no model: Lemonade forwards it to llama.cpp's `/tokenize` on the most "
            "recently used loaded model, which must be a llama.cpp model. With no model loaded, or "
            "when that model is not a llama.cpp model, the answer is `400`. Models that do not "
            "share a tokenizer return different tokens for the same text, so after a request to a "
            "reranker, for example, the reranker's tokenizer answers.",
        };
        s.args = {
            {"content", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available, "Text to tokenize."},
            {"add_special", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
             "Insert special tokens, such as `BOS`. Defaults to `false`."},
            {"parse_special", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
             "Tokenize special tokens; when `false`, they are treated as plain text. Defaults to "
             "`true`."},
            {"with_pieces", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
             "Return each token as `{\"id\", \"piece\"}` instead of a bare id. Defaults to "
             "`false`."},
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = json::parse(R"({
            "type": "object",
            "required": ["tokens"],
            "properties": {
                "tokens": {
                    "type": "array",
                    "description": "Token ids, or {id, piece} objects when with_pieces is true.",
                    "items": {"type": ["integer", "object"]}
                }
            }
        })");
        response.setup = {{"lemonade.load", ResponseFormat::Json}};
        response.example = {{"content", "This is a string to tokenize"}};
        s.responses = {response};
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
