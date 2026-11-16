#include <limits>

#include <lemon/utils/aixlog.hpp>

#include "lemon/router.h"
#include "lemon/server/api_route.h"
#include "recent_llamacpp.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

const RouteError kInvalidSlotId{400, "", "`id` is too large for a 32-bit integer"};

const RouteArg kId{"id", ArgIn::Path, {{"type", "integer"}}, true, Support::Available,
                   "The slot to act on."};
const RouteArg kFilename{"filename", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
                         "File to save to or restore from, relative to `--slot-save-path`. Required for "
                         "`save` and `restore`.", {}, true};
const RouteArg kAction{"action", ArgIn::Query, {{"enum", json::array({"save", "restore", "erase"})}}, true, Support::Available,
                       "`save` writes the slot's prompt cache to `filename`, `restore` reads it back from "
                       "`filename`, and `erase` clears the slot."};

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
        s.args = {kId, kFilename, kAction};

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
        s.errors = join_errors({{kBodyNotJson, kArgMissing, kArgInvalid, kInvalidSlotId},
                                RecentLlamaCpp::errors()});
        s.request_format = RequestFormat::OptionalJson;
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        const json& slot_id_arg = req.arg(kId);
        if (!slot_id_arg.is_number_integer() ||
            slot_id_arg.get<long long>() > std::numeric_limits<int>::max()) {
            const std::string slot_id_str =
                slot_id_arg.is_string() ? slot_id_arg.get<std::string>() : slot_id_arg.dump();
            LOG(ERROR, "Server") << "Invalid slot ID: " << slot_id_str << std::endl;
            write_plain_error(res, kInvalidSlotId, std::string("Invalid slot ID: ") + slot_id_str);
            return;
        }
        const int slot_id = slot_id_arg.get<int>();

        auto action_param = req.value(kAction, std::string());
        RecentLlamaCpp::serve(*ctx_.router, res, "slots " + action_param + " operation", [&] {
            return ctx_.router->slots_action(slot_id, action_param, req.body);
        });
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_slots_action_route(ServerContext& ctx) {
    return std::make_unique<SlotsActionRoute>(ctx);
}

} // namespace lemon
