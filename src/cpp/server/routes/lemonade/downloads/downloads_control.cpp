#include "lemon/server/api_route.h"
#include "lemon/server/download_manager.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

const RouteError kInvalidControlBody{
    400, "", "The body is not valid JSON or not a JSON object, or `id` or `action` is not a string"};
const RouteError kMissingIdOrAction{400, "", "`id` or `action` is missing or empty"};

class DownloadsControlRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "lemonade.downloads_control";
        s.methods = {"POST"};
        s.paths = {"downloads/control"};
        s.summary = "Pause, cancel, or remove server-owned download jobs";
        s.description =
            "Pauses, cancels or removes a server-owned download job listed by "
            "[`GET /v1/downloads`](#get-v1downloads).";
        s.notes = {
            "### Actions\n\n"
            "| Action | Description |\n"
            "|--------|-------------|\n"
            "| `pause` | Requests the worker to stop and keeps the job visible as `paused`. The "
            "worker may briefly report `running=true` while it unwinds. |\n"
            "| `cancel` | Requests the worker to stop and marks the job as `cancelled`. Clients "
            "should wait for `running=false` before deleting partial files. |\n"
            "| `remove` | Removes a stopped job from the server registry. If the worker is still "
            "running, the server keeps the job visible and treats the request as a cancel request "
            "until the worker stops. |",
            "A `pause` or `cancel` for a job that already finished leaves it as it is.",
            "`pause` and `cancel` answer the job's latest snapshot, as does `remove` for a running "
            "job. Removing a stopped job answers `{\"status\": \"ok\"}`, with `\"missing\": true` when "
            "the job was already gone.",
        };
        s.args = {
            {"id", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
             "The job's `id`, for example `model:Qwen3-0.6B-GGUF`."},
            {"action", ArgIn::JsonBody, {{"enum", json::array({"pause", "cancel", "remove"})}}, true,
             Support::Available,
             "What to do with the job; see [Actions](#actions)."},
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        ordered_json removed = ordered_json::parse(R"({
            "type": "object",
            "description": "`remove` of a stopped job, or of an `id` with no job: the job is gone.",
            "required": ["status"],
            "properties": {
                "status": {"const": "ok", "description": "Always `ok`."},
                "missing": {"const": true, "description": "Only when no job had that `id`."}
            }
        })");
        ordered_json snapshot = DownloadManager::job_schema();
        snapshot["description"] = "`pause`, `cancel`, or `remove` of a running job: the job's latest snapshot.";
        response.schema = {{"oneOf", ordered_json::array({removed, snapshot})}};
        s.responses = {response};

        RouteExample example;
        example.name = "pause";
        example.format = ResponseFormat::Json;
        example.setup = {{"lemonade.pull", "job"}};
        example.args = {{"id", "$lemonade.pull/id"}, {"action", "pause"}};

        // No other example starts a job for this model, so the result does not depend on
        // whether an earlier job's worker has stopped yet.
        RouteExample remove_example;
        remove_example.name = "remove";
        remove_example.format = ResponseFormat::Json;
        remove_example.args = {{"id", "model:Qwen3-1.7B-GGUF"}, {"action", "remove"}};
        s.examples = {example, remove_example};
        s.errors = {kInvalidControlBody, kMissingIdOrAction,
                    DownloadManager::kDownloadNotFound, DownloadManager::kUnsupportedAction};
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        try {
            auto request_json = json::parse(req.http.body);
            std::string id = request_json.value("id", "");
            std::string action = request_json.value("action", "");

            if (id.empty() || action.empty()) {
                write_plain_error(res, kMissingIdOrAction, "Both 'id' and 'action' are required");
                return;
            }

            int status = 200;
            json response = ctx_.downloads->control(id, action, status);
            res.status = status;
            res.set_content(response.dump(), "application/json");
        } catch (const std::exception& e) {
            write_plain_error(res, kInvalidControlBody, e.what());
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_downloads_control_route(ServerContext& ctx) {
    return std::make_unique<DownloadsControlRoute>(ctx);
}

} // namespace lemon
