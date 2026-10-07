#include "lemon/jobs/job_manager.h"
#include "lemon/server/api_route.h"

namespace lemon {
namespace {

using json = nlohmann::json;

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
        s.notes = {"An unknown job is answered with `404`."};
        s.args = {
            {"id", ArgIn::Path, {{"type", "string"}}, true, Support::Available, "Job id, from `POST /v1/jobs`."},
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = json::parse(R"({
            "type": "object",
            "required": ["id", "name", "status", "inputs", "context", "steps", "cursor", "created_at"],
            "properties": {
                "id": {"type": "string"},
                "name": {"type": "string"},
                "status": {"enum": ["queued", "running", "paused", "interrupted", "completed", "failed"]},
                "inputs": {"type": "object"},
                "context": {"type": "object", "description": "Each completed step's output under its id, the extracted keys, and inputs."},
                "steps": {"type": "array", "items": {
                    "type": "object",
                    "required": ["id", "op", "params", "status", "duration_ms"],
                    "properties": {
                        "id": {"type": "string"},
                        "op": {"type": "string"},
                        "params": {"type": "object"},
                        "status": {"enum": ["pending", "running", "completed", "failed", "skipped"]},
                        "duration_ms": {"type": "integer"},
                        "error": {"type": "string"},
                        "output": {}
                    }
                }},
                "cursor": {"type": "string", "description": "Id of the current or next step."},
                "created_at": {"type": "string"},
                "started_at": {"type": "string"},
                "finished_at": {"type": "string"},
                "summary": {"type": "string"},
                "error": {"type": "string"}
            }
        })");
        response.setup = {{"lemonade.jobs_create", ResponseFormat::Json}};
        response.example = {{"id", "$lemonade.jobs_create/id"}};
        s.responses = {response};
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        const auto job = ctx_.job_manager->get(req.http.matches[1]);
        if (!job) {
            write_plain_error(res, 404, "unknown job");
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
