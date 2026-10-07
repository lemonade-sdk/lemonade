#include "lemon/jobs/job_manager.h"
#include "lemon/server/api_route.h"

namespace lemon {
namespace {

using json = nlohmann::json;

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
        s.notes = {"An unknown job, or one that is not paused or interrupted, is answered with `404`."};
        s.args = {
            {"id", ArgIn::Path, {{"type", "string"}}, true, Support::Available, "Job id, from `POST /v1/jobs`."},
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = json::parse(R"({
            "type": "object",
            "required": ["status"],
            "properties": {"status": {"const": "resuming"}}
        })");
        response.setup = {{"lemonade.jobs_create", ResponseFormat::Json}, {"lemonade.jobs_interrupt", ResponseFormat::Json}};
        response.example = {{"id", "$lemonade.jobs_create/id"}};
        s.responses = {response};
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        if (!ctx_.job_manager->resume(req.http.matches[1])) {
            write_plain_error(res, 404, "job not found or not resumable");
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
