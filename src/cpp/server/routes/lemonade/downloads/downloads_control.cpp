#include "lemon/server/api_route.h"
#include "lemon/server/download_manager.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

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
            "A `400` answers a missing `id` or `action`, invalid JSON, or an unknown action. A "
            "`pause` or `cancel` for a job that is not listed answers `404`.",
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
            "required": ["status"],
            "properties": {
                "status": {"const": "ok"},
                "missing": {"const": true}
            }
        })");
        ordered_json snapshot = DownloadManager::job_schema();
        response.schema = {{"oneOf", ordered_json::array({removed, snapshot})}};
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.setup = {{"lemonade.pull", "non-streaming"}};
        example.args = {{"id", "$lemonade.pull/id"}, {"action", "pause"}};
        s.examples = {example};
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        try {
            auto request_json = json::parse(req.http.body);
            std::string id = request_json.value("id", "");
            std::string action = request_json.value("action", "");

            if (id.empty() || action.empty()) {
                write_plain_error(res, 400, "Both 'id' and 'action' are required");
                return;
            }

            int status = 200;
            json response = ctx_.downloads->control(id, action, status);
            res.status = status;
            res.set_content(response.dump(), "application/json");
        } catch (const std::exception& e) {
            write_plain_error(res, 400, e.what());
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_downloads_control_route(ServerContext& ctx) {
    return std::make_unique<DownloadsControlRoute>(ctx);
}

} // namespace lemon
