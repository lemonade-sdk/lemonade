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
            "Lists the server-owned download jobs that [`POST /v1/pull`](#post-v1pull) and "
            "[`POST /v1/install`](#post-v1install) start with `stream: true` and "
            "`subscribe: false`, so a client can restore its download manager after a reload or "
            "reconnect.";
        s.notes = {
            "Active, paused, cancelled and failed jobs stay listed until a client removes them "
            "with [`POST /v1/downloads/control`](#post-v1downloadscontrol). Completed jobs stay "
            "listed for 30 seconds, so clients can observe completion and refresh model state.",
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
