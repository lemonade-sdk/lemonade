#include <lemon/utils/aixlog.hpp>

#include "lemon/router.h"
#include "lemon/server/api_route.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

const RouteError kInvalidSlotId{400, "", "`id` is too large for a 32-bit integer"};
const RouteError kMissingAction{400, "", "`action` is missing or empty"};
const RouteError kUnknownAction{400, "", "`action` is not `erase`, `save` or `restore`"};
const RouteError kNoModelLoaded{400, "", "No model is loaded"};
const RouteError kInvalidJson{400, "", "The body is not empty and is not valid JSON"};
const RouteError kNotLlamaCpp{400, "", "The most recently used model is not a llama.cpp model"};
const RouteError kSlotActionFailed{500, "", "The slot action fails unexpectedly"};

class SlotsActionRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "llamacpp.slots_action";
        s.methods = {"POST"};
        s.paths = {"slots/{id}"};
        // Empty: the summary table has a hand-written row per action instead.
        s.summary = "";
        s.modality = "prompt cache";
        s.description =
            "Saves a slot's prompt cache to a file, restores it from one, or erases it, so a "
            "conversation can resume where it left off.";
        s.notes = {
            "Every action needs llama.cpp started with `--slot-save-path`. Set it for every load with `lemonade config set "
            "llamacpp.args=\"--slot-save-path /path/to/slot/saves\"` (see "
            "[Server Configuration](../guide/configuration/README.md)), or for one load with "
            "`llamacpp_args` on [`POST /v1/load`](./lemonade.md#post-v1load).",
        };
        s.args = {
            {"id", ArgIn::Path, {{"type", "integer"}}, true, Support::Available,
             "The slot to act on."},
            {"filename", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
             "File to save to or restore from, relative to `--slot-save-path`. Required for "
             "`save` and `restore`."},
            {"action", ArgIn::Query, {{"enum", json::array({"save", "restore", "erase"})}}, true, Support::Available,
             "`save` writes the slot's prompt cache to `filename`, `restore` reads it back from "
             "`filename`, and `erase` clears the slot."},
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = ordered_json::parse(R"({
            "type": "object",
            "required": ["id_slot"],
            "properties": {
                "id_slot": {"type": "integer", "description": "The slot acted on."},
                "filename": {"type": "string", "description": "`save` and `restore`: the file written or read."},
                "n_saved": {"type": "integer", "description": "`save`: tokens written."},
                "n_restored": {"type": "integer", "description": "`restore`: tokens read."},
                "n_written": {"type": "integer", "description": "`save`: bytes written."},
                "n_read": {"type": "integer", "description": "`restore`: bytes read."},
                "n_erased": {"type": "integer", "description": "`erase`: tokens cleared."},
                "timings": {"type": "object", "description": "`save` and `restore`: how long the file operation took."}
            }
        })");
        s.responses = {response};

        // Every action needs a model loaded with --slot-save-path, and saving needs a prompt in the
        // slot. The generator runs lemond in a temporary directory, so "." keeps files out of the repo.
        const ExampleStep load_with_save_path{
            "lemonade.load", "",
            {{"model_name", "Qwen3-0.6B-GGUF"}, {"llamacpp_args", "--slot-save-path ."}}};

        RouteExample save;
        save.name = "save";
        save.format = ResponseFormat::Json;
        // A chat with pinned sampling and a max_tokens cap leaves the same number of tokens in the
        // slot on every run, so the count the restore example reads back matches the one saved.
        save.setup = {
            load_with_save_path,
            {"openai.chat_completions", "", ordered_json::parse(R"({
                "model": "Qwen3-0.6B-GGUF",
                "messages": [{"role": "user", "content": "What is the population of Paris?"}],
                "temperature": 0,
                "max_tokens": 32
            })")},
        };
        save.args = {{"id", 0}, {"action", "save"}, {"filename", "my_conversation_cache.bin"}};

        RouteExample restore;
        restore.name = "restore";
        restore.format = ResponseFormat::Json;
        restore.setup = {{"llamacpp.slots_action", "save"}};
        restore.args = {{"id", 0}, {"action", "restore"}, {"filename", "my_conversation_cache.bin"}};

        RouteExample erase;
        erase.name = "erase";
        erase.format = ResponseFormat::Json;
        erase.setup = {load_with_save_path};
        erase.args = {{"id", 0}, {"action", "erase"}};

        s.examples = {save, restore, erase};
        s.errors = {kInvalidSlotId, kMissingAction, kUnknownAction, kNoModelLoaded, kInvalidJson,
                    kNotLlamaCpp, kBackendError, kSlotActionFailed};
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
                write_plain_error(res, kInvalidSlotId, std::string("Invalid slot ID: ") + slot_id_str);
                return;
            }

            auto action_param = req.http.get_param_value("action");
            if (action_param.empty()) {
                LOG(ERROR, "Server") << "Missing action parameter for slots POST endpoint" << std::endl;
                write_plain_error(res, kMissingAction, "POST /api/v1/slots/{id} requires action query parameter (e.g., ?action=erase)");
                return;
            }

            if (action_param != "erase" && action_param != "save" && action_param != "restore") {
                LOG(ERROR, "Server") << "Unknown action parameter: " << action_param << std::endl;
                write_plain_error(res, kUnknownAction, std::string("Unknown action: ") + action_param + ". Supported actions: erase, save, restore");
                return;
            }

            // Slot actions operate on backend state, so they need a loaded model rather
            // than a named one.
            if (!ctx_.router->is_model_loaded()) {
                LOG(ERROR, "Server") << "No model loaded for slots " << action_param << " operation" << std::endl;
                write_plain_error(res, kNoModelLoaded, std::string("No model loaded for slots ") + action_param + " operation");
                return;
            }

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

            auto response = ctx_.router->slots_action(slot_id, action_param, request_body);
            if (response.contains("error")) {
                set_error_response(response, res, kBackendError);
                return;
            }

            res.set_content(response.dump(), "application/json");
        } catch (const std::exception& e) {
            LOG(ERROR, "Server") << "ERROR in handle_slots_by_id: " << e.what() << std::endl;
            write_plain_error(res, kSlotActionFailed, e.what());
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_slots_action_route(ServerContext& ctx) {
    return std::make_unique<SlotsActionRoute>(ctx);
}

} // namespace lemon
