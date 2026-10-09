#include <vector>

#include "lemon/jobs/job_manager.h"
#include "lemon/server/api_route.h"

namespace lemon {
namespace {

using json = nlohmann::json;

class JobsCreateRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "lemonade.jobs_create";
        s.methods = {"POST"};
        s.paths = {"jobs"};
        s.summary = "Create a job";
        s.experimental = true;
        s.description =
            "Creates a job: a sequence of server operations that runs in the background, passes "
            "data between steps and branches on results, surviving client disconnects and "
            "server restarts. See [Job Engine](#job-engine).";
        s.notes = {
            "The step graph is validated at creation; an invalid one is answered with `400`. When "
            "the job store holds 50 jobs that are all still active or resumable, the answer is "
            "`429` until one is deleted or finishes.",
        };
        s.args = {
            {"name", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available, "Display name."},
            {"steps", ArgIn::JsonBody, {{"type", "array"}}, false, Support::Available,
             "The steps, in order; see [Recipe: steps](../dev/job-system.md#recipe-steps)."},
            {"definition", ArgIn::JsonBody, {{"type", "object"}}, false, Support::Available,
             "Alternative to `steps`: an object whose `steps` holds the steps."},
            {"inputs", ArgIn::JsonBody, {{"type", "object"}}, false, Support::Available,
             "Values the steps read as `context.inputs`."},
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = json::parse(R"({
            "type": "object",
            "required": ["id"],
            "properties": {"id": {"type": "string", "description": "The job's id."}}
        })");
        response.example = json::parse(R"({
            "name": "example",
            "steps": [{"id": "wait", "op": "sleep", "params": {"ms": 30000}}]
        })");
        s.responses = {response};
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        try {
            auto body = lemon::jobs::json::parse(req.http.body);
            const std::string name = body.value("name", "");
            lemon::jobs::json steps_json = lemon::jobs::json::array();
            if (body.contains("definition") && body["definition"].is_object()
                && body["definition"].contains("steps")) {
                steps_json = body["definition"]["steps"];
            } else if (body.contains("steps")) {
                steps_json = body["steps"];
            }
            std::vector<lemon::jobs::StepRecord> steps;
            if (steps_json.is_array())
                for (const auto& s : steps_json)
                    steps.push_back(lemon::jobs::StepRecord::from_json(s));
            lemon::jobs::json inputs =
                body.contains("inputs") ? body["inputs"] : lemon::jobs::json::object();
            const std::string id = ctx_.job_manager->create(name, std::move(steps), inputs);
            res.status = 202;
            res.set_content(lemon::jobs::json{{"id", id}}.dump(), "application/json");
        } catch (const lemon::jobs::JobError& e) {
            write_plain_error(res, e.status, e.what());
        } catch (const std::exception& e) {
            write_plain_error(res, 400, e.what());
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_jobs_create_route(ServerContext& ctx) {
    return std::make_unique<JobsCreateRoute>(ctx);
}

} // namespace lemon
