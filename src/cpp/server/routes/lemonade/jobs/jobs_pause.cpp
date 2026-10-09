#include "lemon/jobs/job_manager.h"
#include "lemon/server/api_route.h"

namespace lemon {
namespace {

using json = nlohmann::json;

class JobsPauseRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "lemonade.jobs_pause";
        s.methods = {"POST"};
        s.paths = {"jobs/{id}/pause"};
        s.summary = "Pause a job";
        s.experimental = true;
        s.description =
            "Stops a job after its current step and releases the model slot, so queued requests "
            "run. A queued job pauses at once. Models the job loaded stay loaded for the resume.";
        s.notes = {"An unknown job, or one that is not queued or running, is answered with `404`."};
        s.args = {
            {"id", ArgIn::Path, {{"type", "string"}}, true, Support::Available, "Job id, from `POST /v1/jobs`."},
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = json::parse(R"({
            "type": "object",
            "required": ["status"],
            "properties": {"status": {"const": "pausing"}}
        })");
        response.setup = {{"lemonade.jobs_create", ResponseFormat::Json}};
        response.example = {{"id", "$lemonade.jobs_create/id"}};
        s.responses = {response};
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        if (!ctx_.job_manager->pause(req.http.matches[1])) {
            write_plain_error(res, 404, "job not found or not pausable");
            return;
        }
        res.set_content(R"({"status":"pausing"})", "application/json");
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_jobs_pause_route(ServerContext& ctx) {
    return std::make_unique<JobsPauseRoute>(ctx);
}

} // namespace lemon
