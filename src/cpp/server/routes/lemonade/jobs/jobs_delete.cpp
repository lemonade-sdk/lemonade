#include "lemon/jobs/job_manager.h"
#include "lemon/server/api_route.h"

namespace lemon {
namespace {

using json = nlohmann::json;

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
            "Removes a job. An active job is interrupted first, and the models it loaded are "
            "unloaded before it disappears.";
        s.notes = {"An unknown job is answered with `404`."};
        s.args = {
            {"id", ArgIn::Path, {{"type", "string"}}, true, Support::Available, "Job id, from `POST /v1/jobs`."},
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = json::parse(R"({
            "type": "object",
            "required": ["status"],
            "properties": {"status": {"const": "deleted"}}
        })");
        response.setup = {{"lemonade.jobs_create", ResponseFormat::Json}};
        response.example = {{"id", "$lemonade.jobs_create/id"}};
        s.responses = {response};
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        bool active = false;
        if (!ctx_.job_manager->remove(req.http.matches[1], active)) {
            write_plain_error(res, active ? 409 : 404, active ? "job is active" : "unknown job");
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
