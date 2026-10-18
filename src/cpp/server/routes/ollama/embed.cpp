#include <lemon/utils/aixlog.hpp>

#include "lemon/router.h"
#include "lemon/server/api_route.h"
#include "lemon/server/gateway_conversion.h"
#include "lemon/server/openai_args.h"
#include "lemon/server/shared_args.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

const RouteArg kInput{"input", ArgIn::JsonBody, {{"type", json::array({"string", "array"})}}, true, Support::Available,
                      "Text to embed, or an array of texts."};

class EmbedRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "ollama.embed";
        s.methods = {"POST"};
        s.paths = {"/api/embed"};
        s.prefixes = Prefixes::Root;
        s.summary = "Embed one or more texts";
        s.description =
            "Returns one embedding per input text, loading the model on first use.";
        s.args = {gateway::kModel, kInput, ollama::embed::kOptions, model_loading::kCtxSize};

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = ordered_json::parse(R"({
            "type": "object",
            "required": ["model", "embeddings"],
            "properties": {
                "model": {"type": "string", "description": "The model that produced the embeddings."},
                "embeddings": {"type": "array", "items": {"type": "array", "items": {"type": "number"}},
                               "description": "One vector per input, in input order."},
                "total_duration": {"type": "integer", "description": "Always `0`."},
                "load_duration": {"type": "integer", "description": "Always `0`."},
                "prompt_eval_count": {"type": "integer", "description": "Always `0`."}
            }
        })");
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.args = ordered_json::parse(R"({
            "model": "nomic-embed-text-v1-GGUF",
            "input": "Why is the sky blue?"
        })");
        s.examples = {example};
        s.errors = join_errors({{kBodyNotJson, kArgMissing, kArgInvalid}, gateway_load_errors(),
                                {kOllamaBackendError, ApiRoute::kUnexpectedException}});
        s.request_format = RequestFormat::Json;
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        try {
            std::string model = strip_latest_tag(req.arg(gateway::kModel).get<std::string>());

            if (!gateway_ensure_loaded(*ctx_.model_loader, model,
                                       gateway_load_options(req.arg(ollama::embed::kOptions), req.arg(model_loading::kCtxSize)),
                                       ErrorFormat::Plain, res)) {
                return;
            }

            json openai_req;
            openai_req[model_loading::kModel.name] = model;

            openai_req[openai::embeddings::kInput.name] = req.arg(kInput);

            auto openai_response = ctx_.router->embeddings(openai_req);

            if (write_ollama_backend_error(openai_response, res)) return;

            json ollama_res;
            ollama_res["model"] = model;
            ollama_res["embeddings"] = json::array();

            if (openai_response.contains("data") && openai_response["data"].is_array()) {
                for (const auto& item : openai_response["data"]) {
                    if (item.contains("embedding")) {
                        ollama_res["embeddings"].push_back(item["embedding"]);
                    }
                }
            }

            ollama_res["total_duration"] = 0;
            ollama_res["load_duration"] = 0;
            ollama_res["prompt_eval_count"] = 0;

            res.set_content(ollama_res.dump(), "application/json");

        } catch (const std::exception& e) {
            LOG(ERROR, "OllamaApi") << "Error in /api/embed: " << e.what() << std::endl;
            write_plain_error(res, ApiRoute::kUnexpectedException, e.what());
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_ollama_embed_route(ServerContext& ctx) {
    return std::make_unique<EmbedRoute>(ctx);
}

} // namespace lemon
