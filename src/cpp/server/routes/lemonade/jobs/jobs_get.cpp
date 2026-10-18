#include "job_errors.h"
#include "lemon/jobs/job_manager.h"
#include "lemon/server/api_route.h"
#include "lemon/server/shared_args.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

class JobsGetRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "lemonade.jobs_get";
        s.methods = {"GET"};
        s.paths = {"jobs/{id}"};
        s.summary = "Read a job";
        s.experimental = true;
        s.description =
            "Returns a job's full record: its status, the state of each step, and the context "
            "the steps have written.";
        s.args = {job_routes::kId};

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = ordered_json::parse(R"({
            "type": "object",
            "required": ["id", "name", "status", "inputs", "context", "steps", "cursor", "created_at"],
            "properties": {
                "id": {"type": "string", "description": "The job's id."},
                "name": {"type": "string", "description": "The display name the job was created with."},
                "status": {"enum": ["queued", "running", "paused", "interrupted", "completed", "failed"], "description": "The job's state: `queued`, `running`, `paused`, `interrupted`, `completed` or `failed`."},
                "inputs": {"type": "object", "description": "The `inputs` the job was created with."},
                "context": {"type": "object", "description": "Each completed step's output under its id, the extracted keys, and inputs."},
                "steps": {"type": "array", "description": "The job's steps, in order.", "items": {
                    "type": "object",
                    "required": ["id", "op", "params", "status", "duration_ms"],
                    "properties": {
                        "id": {"type": "string", "description": "The step's id."},
                        "op": {"type": "string", "description": "The operation the step runs."},
                        "params": {"type": "object", "description": "The step's parameters."},
                        "status": {"enum": ["pending", "running", "completed", "failed", "skipped"], "description": "The step's state: `pending`, `running`, `completed`, `failed` or `skipped`."},
                        "duration_ms": {"type": "integer", "description": "How long the step ran, in milliseconds."},
                        "error": {"type": "string", "description": "Why the step failed, present after a failure."},
                        "output": {"description": "What the step returned, present once it has output."}
                    }
                }},
                "cursor": {"type": "string", "description": "Id of the current or next step; empty once the job has run its last step."},
                "created_at": {"type": "string", "description": "When the job was created, as an ISO 8601 UTC time."},
                "started_at": {"type": "string", "description": "When the job first ran, present once it has."},
                "finished_at": {"type": "string", "description": "When the job finished, present once it has."},
                "summary": {"type": "string", "description": "Summary text that Lemonade never writes itself: present only when the job's entry in `jobs.json`, in the Lemonade config directory, has one."},
                "error": {"type": "string", "description": "Why the job failed or was interrupted, present when known."}
            }
        })");
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.setup = {{"lemonade.jobs_create", ""}};
        example.args = {{"id", "$lemonade.jobs_create/id"}};
        s.examples = {example};
        s.errors = {kUnknownJob};
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        const auto job = ctx_.job_manager->get(req.value(job_routes::kId, std::string()));
        if (!job) {
            write_unknown_job(res);
            return;
        }
        res.set_content(job->dump(), "application/json");
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_jobs_get_route(ServerContext& ctx) {
    return std::make_unique<JobsGetRoute>(ctx);
}

} // namespace lemon
