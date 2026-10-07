#include <lemon/utils/aixlog.hpp>

#include "lemon/router.h"
#include "lemon/server/api_route.h"

namespace lemon {
namespace {

using json = nlohmann::json;

class SlotsActionRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "llamacpp.slots_action";
        s.methods = {"POST"};
        s.paths = {"slots/{id}"};
        s.summary = "Save, restore or erase the prompt cache of one slot";
        s.description =
            "Saves a slot's prompt cache to a file, restores it from one, or erases it, so a "
            "conversation's context can be persisted and resumed later.";
        s.notes = {
            "The request names no model: Lemonade forwards it to llama.cpp's "
            "`/slots/{id}?action=...` on the most recently used loaded model, which must be a "
            "llama.cpp model. With no model loaded, or when that model is not a llama.cpp model, "
            "the answer is `400`. An `error` from llama.cpp, such as an unknown slot, is returned "
            "with its status.",
            "llama.cpp answers every action with `501` unless the model was loaded with "
            "`--slot-save-path` in its llama.cpp arguments; see [Saving Slots](#saving-slots).",
        };
        s.args = {
            {"id", ArgIn::Path, {{"type", "integer"}}, true, Support::Available,
             "Slot id, from `GET /v1/slots`."},
            {"action", ArgIn::Query, {{"enum", json::array({"save", "restore", "erase"})}}, true, Support::Available,
             "`save` writes the slot's prompt cache to `filename`, `restore` reads it back from "
             "`filename`, and `erase` clears the slot."},
            {"filename", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
             "Required for `save` and `restore`: the cache file, relative to llama.cpp's "
             "`--slot-save-path`."},
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = json::parse(R"({
            "type": "object",
            "required": ["id_slot"],
            "properties": {
                "id_slot": {"type": "integer", "description": "The slot acted on."},
                "filename": {"type": "string", "description": "save and restore: the cache file."},
                "n_saved": {"type": "integer", "description": "save: tokens written to the file."},
                "n_written": {"type": "integer", "description": "save: bytes written."},
                "n_restored": {"type": "integer", "description": "restore: tokens read from the file."},
                "n_read": {"type": "integer", "description": "restore: bytes read."},
                "n_erased": {"type": "integer", "description": "erase: tokens cleared."},
                "timings": {"type": "object", "description": "save and restore: how long the file operation took."}
            }
        })");
        response.setup = {{"lemonade.load", ResponseFormat::Json}};
        response.example = {{"id", 0}, {"action", "erase"}};
        s.responses = {response};
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        try {
            std::string slot_id_str = req.http.matches[1];
            int slot_id;
            try {
                slot_id = std::stoi(slot_id_str);
            } catch (const std::exception& e) {
                LOG(ERROR, "Server") << "Invalid slot ID: " << slot_id_str << std::endl;
                write_plain_error(res, 400, std::string("Invalid slot ID: ") + slot_id_str);
                return;
            }

            auto action_param = req.http.get_param_value("action");
            if (action_param.empty()) {
                LOG(ERROR, "Server") << "Missing action parameter for slots POST endpoint" << std::endl;
                write_plain_error(res, 400, "POST /api/v1/slots/{id} requires action query parameter (e.g., ?action=erase)");
                return;
            }

            if (action_param != "erase" && action_param != "save" && action_param != "restore") {
                LOG(ERROR, "Server") << "Unknown action parameter: " << action_param << std::endl;
                write_plain_error(res, 400, std::string("Unknown action: ") + action_param + ". Supported actions: erase, save, restore");
                return;
            }

            // Slot actions operate on backend state, so they need a loaded model rather
            // than a named one.
            if (!ctx_.router->is_model_loaded()) {
                LOG(ERROR, "Server") << "No model loaded for slots " << action_param << " operation" << std::endl;
                write_plain_error(res, 400, std::string("No model loaded for slots ") + action_param + " operation");
                return;
            }

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

            auto response = ctx_.router->slots_action(slot_id, action_param, request_body);
            if (response.contains("error")) {
                set_error_response(response, res);
                return;
            }

            res.set_content(response.dump(), "application/json");
        } catch (const std::exception& e) {
            LOG(ERROR, "Server") << "ERROR in handle_slots_by_id: " << e.what() << std::endl;
            write_plain_error(res, 500, e.what());
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_slots_action_route(ServerContext& ctx) {
    return std::make_unique<SlotsActionRoute>(ctx);
}

} // namespace lemon
