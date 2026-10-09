#include <lemon/utils/aixlog.hpp>

#include "lemon/router.h"
#include "lemon/server/api_route.h"

namespace lemon {
namespace {

using json = nlohmann::json;

class SlotsRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "llamacpp.slots";
        s.methods = {"GET"};
        s.paths = {"slots"};
        s.summary = "Processing state of the llama.cpp slots";
        s.description =
            "Returns the state of every processing slot of a loaded llama.cpp model. Slots are "
            "parallel processing contexts, each able to serve one request at a time.";
        s.notes = {
            "The request names no model: Lemonade forwards it to llama.cpp's `/slots` on the most "
            "recently used loaded model, which must be a llama.cpp model. With no model loaded, or "
            "when that model is not a llama.cpp model, the answer is `400`.",
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = json::parse(R"({
            "type": "array",
            "items": {
                "type": "object",
                "required": ["id"],
                "properties": {
                    "id": {"type": "integer", "description": "Slot id, used by POST /v1/slots/{id}."},
                    "n_ctx": {"type": "integer", "description": "Context size of the slot."},
                    "id_task": {"type": "integer", "description": "Task the slot is serving, or -1 when idle."},
                    "is_processing": {"type": "boolean", "description": "Whether the slot is serving a request."},
                    "n_prompt_tokens": {"type": "integer", "description": "Tokens in the slot's current prompt."},
                    "params": {"type": "object", "description": "Sampling parameters of the slot's last request."},
                    "next_token": {"type": "array", "description": "Generation state of the current request: has_next_token, n_decoded, n_remain."}
                }
            }
        })");
        response.setup = {{"lemonade.load", ResponseFormat::Json}};
        response.example = json::object();
        s.responses = {response};
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
