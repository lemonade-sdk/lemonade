#include <lemon/utils/aixlog.hpp>

#include "lemon/router.h"
#include "lemon/server/api_route.h"
#include "recent_llamacpp.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

const RouteArg kContent{"content", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
                        "The text to tokenize."};
const RouteArg kAddSpecial{"add_special", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
                           "Insert special tokens, such as `BOS`. Defaults to `false`.", {}, true};
const RouteArg kParseSpecial{"parse_special", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
                             "Tokenize special tokens; `false` treats them as plain text. Defaults to `true`.", {}, true};
const RouteArg kWithPieces{"with_pieces", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
                           "Return each token's text piece with its ID. Defaults to `false`.", {}, true};

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
        s.args = {kContent, kAddSpecial, kParseSpecial, kWithPieces};

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
        s.errors = join_errors({{kBodyNotJson, kArgMissing, kArgInvalid}, RecentLlamaCpp::errors()});
        s.request_format = RequestFormat::OptionalJson;
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        LOG(INFO, "Server") << "POST /api/v1/tokenize" << std::endl;
        RecentLlamaCpp::serve(*ctx_.router, res, "tokenization",
                              [&] { return ctx_.router->tokenize(req.body); });
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_tokenize_route(ServerContext& ctx) {
    return std::make_unique<TokenizeRoute>(ctx);
}

} // namespace lemon
