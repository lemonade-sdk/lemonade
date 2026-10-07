#include "lemon/jobs/job_manager.h"
#include "lemon/server/api_route.h"

namespace lemon {
namespace {

using json = nlohmann::json;

class JobsInterruptRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "lemonade.jobs_interrupt";
        s.methods = {"POST"};
        s.paths = {"jobs/{id}/interrupt"};
        s.summary = "Interrupt a job";
        s.experimental = true;
        s.description =
            "Cancels a job's current step now, aborting an in-flight load or chat. The step "
            "returns to pending, so resuming re-runs it, and the models the job loaded are unloaded.";
        s.notes = {"An unknown job, or one that is not queued or running, is answered with `404`."};
        s.args = {
            {"id", ArgIn::Path, {{"type", "string"}}, true, Support::Available, "Job id, from `POST /v1/jobs`."},
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = json::parse(R"({
            "type": "object",
            "required": ["status"],
            "properties": {"status": {"const": "interrupting"}}
        })");
        response.setup = {{"lemonade.jobs_create", ResponseFormat::Json}};
        response.example = {{"id", "$lemonade.jobs_create/id"}};
        s.responses = {response};
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        if (!ctx_.job_manager->interrupt(req.http.matches[1])) {
            write_plain_error(res, 404, "job not found or not interruptible");
            return;
        }
        res.set_content(R"({"status":"interrupting"})", "application/json");
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_jobs_interrupt_route(ServerContext& ctx) {
    return std::make_unique<JobsInterruptRoute>(ctx);
}

} // namespace lemon
