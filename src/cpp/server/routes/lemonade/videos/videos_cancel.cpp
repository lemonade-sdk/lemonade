#include "video_shared.h"
#include "lemon/jobs/job_manager.h"
#include "lemon/server/api_route.h"

namespace lemon {
namespace {

using ordered_json = nlohmann::ordered_json;

class VideosCancelRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "lemonade.videos_cancel";
        s.methods = {"POST"};
        s.paths = {"videos/{id}/cancel"};
        s.summary = "Cancel a video";
        s.experimental = true;
        s.description =
            "Stops a queued or running video and keeps its record, which then reports `canceled`.";
        s.notes = {
            "A clip still queued is cancelled straight away. stable-diffusion.cpp cannot stop a clip "
            "once frames are being produced, so cancelling one stops its backend process; the video "
            "reports `canceled` once the process has exited, and the next request reloads the model.",
        };
        s.args = {video_routes::kId};

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = ordered_json::parse(R"({
            "type": "object",
            "required": ["status"],
            "properties": {"status": {"const": "canceling", "description": "Always `canceling`."}}
        })");
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.setup = {{"lemonade.videos_create", ""}};
        example.args = {{"id", "$lemonade.videos_create/id"}};
        s.examples = {example};
        s.errors = {kVideoNotActive};
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        if (!ctx_.job_manager->interrupt(req.value(video_routes::kId, std::string()))) {
            write_plain_error(res, kVideoNotActive, "video not found or not cancellable");
            return;
        }
        res.set_content(R"({"status":"canceling"})", "application/json");
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_videos_cancel_route(ServerContext& ctx) {
    return std::make_unique<VideosCancelRoute>(ctx);
}

} // namespace lemon
