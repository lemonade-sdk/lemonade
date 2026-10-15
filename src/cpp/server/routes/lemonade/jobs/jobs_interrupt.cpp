#include "lemon/jobs/job_manager.h"
#include "lemon/server/api_route.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

const RouteError kJobNotFound{404, "", "`id` names no job, a deleted one, or one that is not queued or running"};

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
        s.args = {
            {"id", ArgIn::Path, {{"type", "string"}}, true, Support::Available, "Job id, from [`POST /v1/jobs`](#post-v1jobs)."},
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = ordered_json::parse(R"({
            "type": "object",
            "required": ["status"],
            "properties": {"status": {"const": "interrupting", "description": "Always `interrupting`."}}
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
        if (!ctx_.job_manager->interrupt(req.http.matches[1])) {
            write_plain_error(res, kJobNotFound, "job not found or not interruptible");
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
