#include "lemon/jobs/job_manager.h"
#include "lemon/server/api_route.h"

namespace lemon {
namespace {

using json = nlohmann::json;

class JobsListRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "lemonade.jobs_list";
        s.methods = {"GET"};
        s.paths = {"jobs"};
        s.summary = "List jobs";
        s.experimental = true;
        s.description = "Lists a summary of every job, active or finished.";

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = json::parse(R"({
            "type": "object",
            "required": ["jobs"],
            "properties": {
                "jobs": {"type": "array", "items": {
                    "type": "object",
                    "required": ["id", "name", "status", "created_at", "progress"],
                    "properties": {
                        "id": {"type": "string"},
                        "name": {"type": "string"},
                        "status": {"enum": ["queued", "running", "paused", "interrupted", "completed", "failed"]},
                        "created_at": {"type": "string"},
                        "finished_at": {"type": "string"},
                        "progress": {
                            "type": "object",
                            "properties": {
                                "cursor": {"type": "string", "description": "Id of the current or next step."},
                                "completed": {"type": "integer", "description": "Steps that need no more work: completed, skipped, or failed with the failure handled."},
                                "step_count": {"type": "integer"}
                            }
                        },
                        "summary": {"type": "string"},
                        "error": {"type": "string"}
                    }
                }}
            }
        })");
        response.setup = {{"lemonade.jobs_create", ResponseFormat::Json}};
        response.example = json::object();
        s.responses = {response};
        return s;
    }

    void handle(RouteRequest&, httplib::Response& res) override {
        res.set_content(lemon::jobs::json{{"jobs", ctx_.job_manager->list()}}.dump(),
                        "application/json");
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_jobs_list_route(ServerContext& ctx) {
    return std::make_unique<JobsListRoute>(ctx);
}

} // namespace lemon
