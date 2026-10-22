#include "job_errors.h"
#include "lemon/jobs/job_manager.h"
#include "lemon/server/api_route.h"
#include "lemon/server/shared_args.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

class JobsDeleteRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "lemonade.jobs_delete";
        s.methods = {"DELETE"};
        s.paths = {"jobs/{id}"};
        s.summary = "Delete a job";
        s.experimental = true;
        s.description =
            "Removes a job at once, even a running one, which is interrupted. The models a "
            "running or paused job loaded are unloaded in the background; a finished job's "
            "models stay loaded.";
        s.args = {job_routes::kId};

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = ordered_json::parse(R"({
            "type": "object",
            "required": ["status"],
            "properties": {"status": {"const": "deleted", "description": "Always `deleted`."}}
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
        bool active = false;
        if (!ctx_.job_manager->remove(req.value(job_routes::kId, std::string()), active)) {
            write_unknown_job(res);
            return;
        }
        res.set_content(R"({"status":"deleted"})", "application/json");
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_jobs_delete_route(ServerContext& ctx) {
    return std::make_unique<JobsDeleteRoute>(ctx);
}

} // namespace lemon
