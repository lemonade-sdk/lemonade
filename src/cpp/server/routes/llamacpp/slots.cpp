#include <lemon/utils/aixlog.hpp>

#include "lemon/router.h"
#include "lemon/server/api_route.h"

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
        s.notes = {
            "With no model loaded, or when the most recently used one is not a llama.cpp model, "
            "the answer is `400`.",
        };

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
                            "n_remain": {"type": "integer", "description": "Number of tokens remaining to generate"},
                            "n_decoded": {"type": "integer", "description": "Number of tokens already decoded"}
                        }
                    }},
                    "id_task": {"type": "integer", "description": "The task the slot is processing, or processed last when idle."},
                    "n_ctx": {"type": "integer", "description": "Context size of the slot."},
                    "is_processing": {"type": "boolean", "description": "Whether the slot is serving a request."},
                    "n_prompt_tokens": {"type": "integer", "description": "Tokens in the slot's current prompt."},
                    "params": {"type": "object", "description": "Sampling parameters of the slot's last request."}
                }
            }
        })json");
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.setup = {{"lemonade.load", ""}};
        example.args = json::object();
        s.examples = {example};
        return s;
    }

    void handle(RouteRequest&, httplib::Response& res) override {
        try {
            // Slots are backend state, so they need a loaded model rather than a named one.
            if (!ctx_.router->is_model_loaded()) {
                LOG(ERROR, "Server") << "No model loaded for slots query" << std::endl;
                write_plain_error(res, 400, "No model loaded for slots query");
                return;
            }

            auto response = ctx_.router->get_slots();
            if (response.contains("error")) {
                set_error_response(response, res);
                return;
            }

            res.set_content(response.dump(), "application/json");
        } catch (const std::exception& e) {
            LOG(ERROR, "Server") << "ERROR in handle_slots: " << e.what() << std::endl;
            write_plain_error(res, 500, e.what());
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_slots_route(ServerContext& ctx) {
    return std::make_unique<SlotsRoute>(ctx);
}

} // namespace lemon
