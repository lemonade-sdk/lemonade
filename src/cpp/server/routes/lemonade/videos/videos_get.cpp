#include "video_shared.h"
#include "lemon/server/api_route.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

class VideosGetRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "lemonade.videos_get";
        s.methods = {"GET"};
        s.paths = {"videos/{id}"};
        s.summary = "Read a video's status";
        s.experimental = true;
        s.description =
            "Returns a video's status, and once it completes, the clip's format and length. The clip "
            "itself is served by [`GET /v1/videos/{id}/content`](#get-v1videosidcontent).";
        s.args = {video_routes::kId};

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = ordered_json::parse(R"({
            "type": "object",
            "required": ["id", "status"],
            "properties": {
                "id": {"type": "string", "description": "The video's id."},
                "status": {"enum": ["queued", "running", "paused", "canceled", "completed", "failed"], "description": "The video's state: `queued`, `running`, `canceled`, `completed` or `failed`, or `paused` when paused through the job routes."},
                "model": {"type": "string", "description": "The model generating the video."},
                "created_at": {"type": "string", "description": "When the job was created, as an ISO 8601 UTC time."},
                "started_at": {"type": "string", "description": "When generation started, as an ISO 8601 UTC time."},
                "completed_at": {"type": "string", "description": "When the video completed, failed or was canceled, as an ISO 8601 UTC time."},
                "error": {"type": "string", "description": "Why generation failed, present after a failure."},
                "mime_type": {"type": "string", "description": "The clip's media type, present once completed."},
                "output_format": {"type": "string", "description": "The clip's container format, present once completed."},
                "frame_count": {"type": "integer", "description": "Frames in the clip, present once completed."},
                "fps": {"type": "integer", "description": "The clip's frame rate, present once completed."},
                "duration": {"type": "number", "description": "The clip's length in seconds, `frame_count` over `fps`, present once completed."}
            }
        })");
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.setup = {{"lemonade.videos_create", ""}};
        example.args = {{"id", "$lemonade.videos_create/id"}};
        s.examples = {example};
        s.errors = {kUnknownVideo};
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        const auto job = find_video(ctx_, req.value(video_routes::kId, std::string()));
        if (!job) {
            write_unknown_video(res);
            return;
        }
        json out = {{"id", job->value("id", "")}, {"status", video_status(job->value("status", ""))}};
        if (job->contains("created_at")) out["created_at"] = (*job)["created_at"];
        if (job->contains("started_at")) out["started_at"] = (*job)["started_at"];
        if (job->contains("finished_at")) out["completed_at"] = (*job)["finished_at"];
        if (job->contains("steps") && (*job)["steps"].is_array() && !(*job)["steps"].empty()) {
            const auto& step = (*job)["steps"][0];
            if (step.contains("params") && step["params"].is_object())
                out["model"] = step["params"].value("model", "");
            if (step.contains("error") && !step["error"].empty()) out["error"] = step["error"];
            // The status payload drops the encoded bytes; they come from the content route.
            if (step.contains("output") && step["output"].is_object()) {
                for (const char* key : {"mime_type", "output_format", "frame_count", "fps"}) {
                    if (step["output"].contains(key)) out[key] = step["output"][key];
                }
                if (out.contains("frame_count") && out["frame_count"].is_number() &&
                    out.contains("fps") && out["fps"].is_number() && out["fps"].get<double>() > 0) {
                    out["duration"] = out["frame_count"].get<double>() / out["fps"].get<double>();
                }
            }
        }
        res.set_content(out.dump(), "application/json");
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_videos_get_route(ServerContext& ctx) {
    return std::make_unique<VideosGetRoute>(ctx);
}

} // namespace lemon
