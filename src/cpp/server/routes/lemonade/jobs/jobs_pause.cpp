#include "lemon/jobs/job_manager.h"
#include "lemon/server/api_route.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

const RouteError kJobNotFound{404, "", "`id` names no job, a deleted one, or one that is not queued or running"};

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
        s.args = {
            {"id", ArgIn::Path, {{"type", "string"}}, true, Support::Available, "Job id, from [`POST /v1/jobs`](#post-v1jobs)."},
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = ordered_json::parse(R"({
            "type": "object",
            "required": ["status"],
            "properties": {"status": {"const": "pausing", "description": "Always `pausing`."}}
        })");
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.setup = {{"lemonade.jobs_create", ""}};
        example.args = {{"id", "$lemonade.jobs_create/id"}};
        s.examples = {example};
        s.errors = {kJobNotFound};
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        if (!ctx_.job_manager->pause(req.http.matches[1])) {
            write_plain_error(res, kJobNotFound, "job not found or not pausable");
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
