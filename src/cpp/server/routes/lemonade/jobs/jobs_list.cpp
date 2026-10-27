#include "lemon/jobs/job_manager.h"
#include "lemon/server/api_route.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

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
        response.schema = ordered_json::parse(R"({
            "type": "object",
            "required": ["jobs"],
            "properties": {
                "jobs": {"type": "array", "description": "Every job not deleted or evicted, newest first.", "items": {
                    "type": "object",
                    "required": ["id", "name", "status", "created_at", "progress"],
                    "properties": {
                        "id": {"type": "string", "description": "The job's id."},
                        "name": {"type": "string", "description": "The display name the job was created with."},
                        "status": {"enum": ["queued", "running", "paused", "interrupted", "completed", "failed"], "description": "The job's state: `queued`, `running`, `paused`, `interrupted`, `completed` or `failed`."},
                        "created_at": {"type": "string", "description": "When the job was created, as an ISO 8601 UTC time."},
                        "finished_at": {"type": "string", "description": "When the job finished, present once it has."},
                        "progress": {
                            "type": "object",
                            "description": "How far through its steps the job is.",
                            "properties": {
                                "cursor": {"type": "string", "description": "Id of the current or next step; empty once the job has run its last step."},
                                "completed": {"type": "integer", "description": "Steps that need no more work: completed, skipped, or failed with the failure handled."},
                                "step_count": {"type": "integer", "description": "How many steps the job has."}
                            }
                        },
                        "summary": {"type": "string", "description": "Summary text that Lemonade never writes itself: present only when the job's entry in `jobs.json`, in the Lemonade config directory, has one."},
                        "error": {"type": "string", "description": "Why the job failed or was interrupted, present when known."}
                    }
                }}
            }
        })");
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.setup = {{"lemonade.jobs_create", ""}};
        example.args = json::object();
        s.examples = {example};
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
