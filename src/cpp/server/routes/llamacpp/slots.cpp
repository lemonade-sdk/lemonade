#include "lemon/router.h"
#include "lemon/server/api_route.h"
#include "recent_llamacpp.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

class SlotsRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "llamacpp.slots";
        s.methods = {"GET"};
        s.paths = {"slots"};
        s.summary = "Read each slot's processing state";
        s.modality = "slots state";
        s.description =
            "Returns the state of each processing slot in the llama.cpp server. Slots are the "
            "parallel contexts that serve concurrent requests.";

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = ordered_json::parse(R"json({
            "type": "array",
            "items": {
                "type": "object",
                "required": ["id"],
                "properties": {
                    "id": {"type": "integer", "description": "The slot's ID."},
                    "next_token": {"type": "array", "description": "Generation state of the slot's current request.", "items": {
                        "type": "object",
                        "properties": {
                            "has_next_token": {"type": "boolean", "description": "Whether more tokens are expected"},
                            "has_new_line": {"type": "boolean", "description": "Whether the request's generated text contains a newline."},
                            "n_remain": {"type": "integer", "description": "Number of tokens remaining to generate"},
                            "n_decoded": {"type": "integer", "description": "Number of tokens already decoded"}
                        }
                    }},
                    "id_task": {"type": "integer", "description": "The task the slot is processing, or processed last when idle."},
                    "n_ctx": {"type": "integer", "description": "Context size of the slot."},
                    "is_processing": {"type": "boolean", "description": "Whether the slot is serving a request."},
                    "n_prompt_tokens": {"type": "integer", "description": "Tokens in the slot's current prompt."},
                    "n_prompt_tokens_cache": {"type": "integer", "description": "Prompt tokens reused from the slot's cache instead of processed again."},
                    "n_prompt_tokens_processed": {"type": "integer", "description": "Prompt tokens processed for the current request."},
                    "params": {"type": "object", "description": "Sampling parameters of the slot's last request."},
                    "speculative": {"type": "boolean", "description": "Whether the slot uses speculative decoding."}
                }
            }
        })json");
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.setup = {{"lemonade.load", "model"}};
        example.args = json::object();
        s.examples = {example};
        s.errors = RecentLlamaCpp::errors();
        return s;
    }

    void handle(RouteRequest&, httplib::Response& res) override {
        RecentLlamaCpp::serve(*ctx_.router, res, "slots query",
                              [this] { return ctx_.router->get_slots(); });
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_slots_route(ServerContext& ctx) {
    return std::make_unique<SlotsRoute>(ctx);
}

} // namespace lemon
