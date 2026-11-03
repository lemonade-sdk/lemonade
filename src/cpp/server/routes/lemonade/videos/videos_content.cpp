#include "video_shared.h"
#include "lemon/jobs/job_manager.h"
#include "lemon/server/api_route.h"
#include "lemon/utils/json_utils.h"

namespace lemon {
namespace {

class VideosContentRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "lemonade.videos_content";
        s.methods = {"GET"};
        s.paths = {"videos/{id}/content"};
        s.summary = "Download a generated video";
        s.experimental = true;
        s.description =
            "Returns the encoded clip as raw bytes, with its media type, once the video's status is "
            "`completed`.";
        s.args = {video_routes::kId};

        RouteResponse response;
        response.format = ResponseFormat::Binary;
        s.responses = {response};
        s.errors = {kUnknownVideo, kVideoNotReady, kVideoNoOutput};
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        const auto job = ctx_.job_manager->get(req.value(video_routes::kId, std::string()));
        if (!job) {
            write_unknown_video(res);
            return;
        }
        const std::string status = job->value("status", "");
        if (status != "completed") {
            write_plain_error(res, kVideoNotReady, "video is not ready: " + video_status(status));
            return;
        }
        if (!job->contains("steps") || !(*job)["steps"].is_array() || (*job)["steps"].empty() ||
            !(*job)["steps"][0].contains("output")) {
            write_plain_error(res, kVideoNoOutput, "video job has no output");
            return;
        }
        const auto& output = (*job)["steps"][0]["output"];
        if (!output.is_object() || !output.contains("b64_json") || !output["b64_json"].is_string()) {
            write_plain_error(res, kVideoNoOutput, "video job has no output");
            return;
        }
        res.set_content(utils::JsonUtils::base64_decode(output["b64_json"].get<std::string>()),
                        output.value("mime_type", "video/webm"));
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_videos_content_route(ServerContext& ctx) {
    return std::make_unique<VideosContentRoute>(ctx);
}

} // namespace lemon
