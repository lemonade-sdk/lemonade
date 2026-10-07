#include "lemon/server/api_route.h"
#include "lemon/server/download_manager.h"

namespace lemon {
namespace {

using json = nlohmann::json;

class DownloadsControlRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "lemonade.downloads_control";
        s.methods = {"POST"};
        s.paths = {"downloads/control"};
        s.summary = "Pause, cancel, or remove server-owned model download jobs";
        s.description = "Pauses, cancels or removes a server-owned download job.";
        s.notes = {
            "`pause` asks the worker to stop and keeps the job listed as `paused`. `cancel` asks "
            "it to stop and marks the job `cancelled`; clients should wait for `running: false` "
            "before deleting partial files. Either may briefly report `running: true` while the "
            "worker unwinds, and a job that already finished is left as it is.",
            "`remove` drops a stopped job from the list. While its worker is still running, the "
            "job stays listed and the request is treated as a `cancel` until the worker stops. "
            "Removing a job that is not listed succeeds with `missing: true`.",
            "A `400` answers a missing `id` or `action`, invalid JSON, or an unknown action. A "
            "`pause` or `cancel` for a job that is not listed answers `404`.",
        };
        s.args = {
            {"id", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
             "Download id from `POST /v1/pull` or `GET /v1/downloads`, e.g. "
             "`model:Qwen3-0.6B-GGUF`."},
            {"action", ArgIn::JsonBody, {{"enum", json::array({"pause", "cancel", "remove"})}}, true,
             Support::Available, "`pause`, `cancel` or `remove`."},
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        json removed = json::parse(R"({
            "type": "object",
            "required": ["status"],
            "properties": {
                "status": {"const": "ok"},
                "missing": {"const": true, "description": "Only when the job was not listed."}
            }
        })");
        json snapshot = DownloadManager::job_schema();
        snapshot["description"] = "pause, cancel, and remove of a running job: the job's snapshot.";
        response.schema = {{"oneOf", json::array({removed, snapshot})}};
        response.setup = {{"lemonade.pull", ResponseFormat::Json}};
        response.example = {{"id", "$lemonade.pull/id"}, {"action", "remove"}};
        s.responses = {response};
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
