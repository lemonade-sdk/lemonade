#include "lemon/server/api_route.h"
#include "lemon/server/download_manager.h"

namespace lemon {
namespace {

using json = nlohmann::json;

class DownloadsRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "lemonade.downloads";
        s.methods = {"GET"};
        s.paths = {"downloads"};
        s.summary = "List server-owned model download jobs";
        s.description =
            "List server-owned model download jobs that were started with `POST /v1/pull` or "
            "`POST /v1/install` using `stream=true` and `subscribe=false`.\n\n"
            "This endpoint is intended for clients that need to restore download-manager state "
            "after a reload or reconnect. Active, paused, cancelled, and errored jobs remain "
            "visible until the client removes them. Completed jobs remain visible briefly so "
            "clients can observe completion and refresh model state.";
        s.notes = {
            "Completed jobs stay listed for 30 seconds.",
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = {{"type", "array"}, {"items", DownloadManager::job_schema()}};
        response.setup = {{"lemonade.pull", ResponseFormat::Json}};
        response.example = json::object();
        s.responses = {response};
        s.quiet_log = true;
        return s;
    }

    void handle(RouteRequest&, httplib::Response& res) override {
        res.set_content(ctx_.downloads->list().dump(), "application/json");
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_downloads_route(ServerContext& ctx) {
    return std::make_unique<DownloadsRoute>(ctx);
}

} // namespace lemon
