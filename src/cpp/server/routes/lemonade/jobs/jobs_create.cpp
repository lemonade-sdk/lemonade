#include <vector>

#include "lemon/jobs/job_manager.h"
#include "lemon/server/api_route.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

const RouteError kInvalidSteps{
    400, "", "The step graph is invalid, for example no steps, a duplicate step id or an unknown `op`"};
const RouteError kJobLimit{429, "", "All 50 jobs in the store are still active or resumable"};

// StepRecord::from_json also reads the fields a stored job carries, such as `status`.
json step_list_schema() {
    return json::parse(R"({"type": "array", "items": {"type": "object", "properties": {
        "id": {"type": "string"}, "op": {"type": "string"}, "params": {"type": "object"},
        "when": {"type": "string"}, "extract": {"type": "object"}, "on_done": {"type": "string"},
        "branch": {"type": "array", "items": {"type": "object", "properties": {
            "when": {"type": "string"}, "goto": {"type": "string"}}}},
        "on_fail": {"type": "string"}, "failure_handled": {"type": "boolean"},
        "status": {"type": "string"}, "duration_ms": {"type": "integer"}, "error": {"type": "string"}
    }}})");
}

const RouteArg kName{"name", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
                     "Display name. Defaults to `\"\"`."};
const RouteArg kSteps{"steps", ArgIn::JsonBody, step_list_schema(), false, Support::Available,
                      "The steps, in order, at least one; see "
                      "[Recipe: steps](../dev/job-system.md#recipe-steps). Required unless `definition` "
                      "holds them."};
const RouteArg kDefinition{"definition", ArgIn::JsonBody,
                           {{"type", "object"}, {"properties", {{"steps", step_list_schema()}}}}, false, Support::Available,
                           "Alternative to `steps`: an object whose `steps` holds the steps. Takes precedence over "
                           "`steps`."};
const RouteArg kInputs{"inputs", ArgIn::JsonBody, json::object(), false, Support::Available,
                       "Values the steps read as `context.inputs`."};

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
            "Creates a job: a sequence of server operations that runs in the background, passing "
            "data between steps and branching on results. A job survives client disconnects and "
            "server restarts (a restart leaves an active job interrupted), and can be paused, "
            "interrupted, resumed, read and deleted.";
        s.notes = {
            "The job store holds 50 jobs: a new job evicts the oldest finished one.",
            "A job with any `load`, `unload` or `chat` step holds the Router's exclusive slot "
            "for as long as it runs, so normal traffic queues behind it.",
            "[`docs/dev/job-system.md`](../dev/job-system.md) specifies the step schema, op set and "
            "lifecycle, and [`docs/dev/job-expression-language.md`](../dev/job-expression-language.md) "
            "the `when`/`branch` expression grammar.",
        };
        s.args = {kName, kSteps, kDefinition, kInputs};

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = ordered_json::parse(R"({
            "type": "object",
            "required": ["id"],
            "properties": {"id": {"type": "string", "description": "The job's id."}}
        })");
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.args = ordered_json::parse(R"({
            "name": "example",
            "steps": [{"id": "wait", "op": "sleep", "params": {"ms": 30000}}]
        })");
        s.examples = {example};
        s.errors = {kBodyNotJson, kArgInvalid, kInvalidSteps, kJobLimit};
        s.request_format = RequestFormat::Json;
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        try {
            // Parsed again as ordered_json, so `GET jobs/{id}` echoes the keys in the client's order.
            auto body = lemon::jobs::json::parse(req.http.body);
            const std::string name = req.value(kName, std::string());
            lemon::jobs::json steps_json = lemon::jobs::json::array();
            if (body.contains(kDefinition.name) && body[kDefinition.name].is_object()
                && body[kDefinition.name].contains("steps")) {
                steps_json = body[kDefinition.name]["steps"];
            } else if (body.contains(kSteps.name)) {
                steps_json = body[kSteps.name];
            }
            std::vector<lemon::jobs::StepRecord> steps;
            if (steps_json.is_array())
                for (const auto& s : steps_json)
                    steps.push_back(lemon::jobs::StepRecord::from_json(s));
            lemon::jobs::json inputs =
                body.contains(kInputs.name) ? body[kInputs.name] : lemon::jobs::json::object();
            const std::string id = ctx_.job_manager->create(name, std::move(steps), inputs);
            res.status = 202;
            res.set_content(lemon::jobs::json{{"id", id}}.dump(), "application/json");
        } catch (const lemon::jobs::JobError& e) {
            write_plain_error(res, e.status == kJobLimit.status ? kJobLimit : kInvalidSteps,
                              e.what());
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_jobs_create_route(ServerContext& ctx) {
    return std::make_unique<JobsCreateRoute>(ctx);
}

} // namespace lemon
