#include "job_errors.h"
#include "lemon/jobs/job_manager.h"
#include "lemon/server/api_route.h"
#include "lemon/server/shared_args.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

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
        s.args = {job_routes::kId};

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
        s.errors = {kJobNotActive};
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        if (!ctx_.job_manager->pause(req.value(job_routes::kId, std::string()))) {
            write_job_not_active(res, "pausable");
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
