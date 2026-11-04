#include "lemon/jobs/job_manager.h"
#include "lemon/server/api_route.h"
#include "lemon/server/shared_args.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

const RouteError kJobNotFound{404, "", "`id` names no job, a deleted one, or one that is not paused or interrupted"};

class JobsResumeRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "lemonade.jobs_resume";
        s.methods = {"POST"};
        s.paths = {"jobs/{id}/resume"};
        s.summary = "Resume a job";
        s.experimental = true;
        s.description =
            "Continues a paused job at its next step, or re-runs an interrupted job's pending "
            "step after reloading the models the interrupt unloaded.";
        s.args = {job_routes::kId};

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = ordered_json::parse(R"({
            "type": "object",
            "required": ["status"],
            "properties": {"status": {"const": "resuming", "description": "Always `resuming`."}}
        })");
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.setup = {{"lemonade.jobs_create", ""}, {"lemonade.jobs_interrupt", ""}};
        example.args = {{"id", "$lemonade.jobs_create/id"}};
        s.examples = {example};
        s.errors = {kJobNotFound};
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        if (!ctx_.job_manager->resume(req.value(job_routes::kId, std::string()))) {
            write_plain_error(res, kJobNotFound, "job not found or not resumable");
            return;
        }
        res.set_content(R"({"status":"resuming"})", "application/json");
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_jobs_resume_route(ServerContext& ctx) {
    return std::make_unique<JobsResumeRoute>(ctx);
}

} // namespace lemon
