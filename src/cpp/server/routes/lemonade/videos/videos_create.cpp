#include <set>
#include <string>

#include <lemon/utils/aixlog.hpp>

#include "lemon/jobs/job_manager.h"
#include "lemon/server/api_route.h"
#include "lemon/server/shared_args.h"
#include "lemon/server/video_store.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

const RouteArg kPrompt{"prompt", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
                       "The text description of the video to generate."};
const RouteArg kNegativePrompt{"negative_prompt", ArgIn::JsonBody, {{"type", "string"}}, false,
                               Support::Available, "Text describing what to avoid."};
const RouteArg kVideoFrames{"video_frames", ArgIn::JsonBody, {{"type", "integer"}, {"minimum", 1}}, false,
                            Support::Available,
                            "Number of frames to generate. Defaults to the model's `video_frames` recipe "
                            "option, else `33`."};
const RouteArg kFps{"fps", ArgIn::JsonBody, {{"type", "integer"}, {"minimum", 1}}, false, Support::Available,
                    "Frames per second of the returned clip. Defaults to the model's `fps` recipe option, "
                    "else `16`."};
const RouteArg kDuration{"duration", ArgIn::JsonBody, {{"type", "number"}, {"minimum", 0}}, false,
                         Support::Available,
                         "Length of the clip in seconds. Approximate: it is converted to frames at `fps` and "
                         "rounded to the nearest count the model takes, which is 4n+1 for Wan. "
                         "`video_frames` wins when both are given."};
const RouteArg kWidth{"width", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
                      "Output width in pixels. Video models are sensitive to resolutions they were not "
                      "trained on. Defaults to the model's own."};
const RouteArg kHeight{"height", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
                       "Output height in pixels. Defaults to the model's own."};
const RouteArg kCfgScale{"cfg_scale", ArgIn::JsonBody, {{"type", "number"}}, false, Support::Available,
                         "Classifier-free guidance scale. Defaults to the model's own."};
const RouteArg kSamplingMethod{"sampling_method", ArgIn::JsonBody, {{"type", "string"}}, false,
                               Support::Available, "Sampler name, such as `euler`. Defaults to the model's own."};
const RouteArg kFlowShift{"flow_shift", ArgIn::JsonBody, {{"type", "number"}}, false, Support::Available,
                          "Flow shift, used by flow-matching models such as Wan. Defaults to the model's own."};
const RouteArg kOutputFormat{"output_format", ArgIn::JsonBody, {{"enum", json::array({"webm", "webp", "avi"})}},
                             false, Support::Available,
                             "Container format of the clip: `webm`, `webp` or `avi`. Defaults to `webm`."};

class VideosCreateRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "lemonade.videos_create";
        s.methods = {"POST"};
        s.paths = {"videos"};
        s.summary = "Generate a video from a text prompt";
        s.experimental = true;
        s.description =
            "Starts generating a video from a text prompt and returns a handle at once. Generation "
            "runs for minutes, so it is a job rather than a held connection: poll "
            "[`GET /v1/videos/{id}`](#get-v1videosid), then fetch the clip from "
            "[`GET /v1/videos/{id}/content`](#get-v1videosidcontent). It serves "
            "[stable-diffusion.cpp](https://github.com/leejet/stable-diffusion.cpp) video models such "
            "as `Wan2.1-T2V-1.3B` and is a Lemonade extension.";
        s.notes = {
            "The model loads when the job starts, not during this request.",
            "The clip is kept on disk under the cache directory until its job is evicted or deleted.",
            "stable-diffusion.cpp generates a single frame when no frame count is given, and video "
            "models need a few dozen frames before they animate rather than drift, so Lemonade sends "
            "33 by default.",
            "A video is an ordinary job, so it is also listed under [`GET /v1/jobs`](#get-v1jobs) and "
            "accepts the job control routes.",
            "GPU only, in practice. A 33-frame 832x480 clip takes about 5 minutes on a discrete GPU.",
        };
        s.args = {
            kPrompt, model_loading::kModel, kNegativePrompt, kDuration, kVideoFrames, kFps, kWidth, kHeight,
            inference::kSteps, kCfgScale, kSamplingMethod, kFlowShift, inference::kSeed, kOutputFormat
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = ordered_json::parse(R"({
            "type": "object",
            "required": ["id", "status", "model"],
            "properties": {
                "id": {"type": "string", "description": "The video's id."},
                "status": {"enum": ["queued", "running"], "description": "The job's state: `queued` or `running`."},
                "model": {"type": "string", "description": "The model generating the video."},
                "created_at": {"type": "string", "description": "When the job was created, as an ISO 8601 UTC time."}
            }
        })");
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.args = ordered_json::parse(R"({
            "model": "Wan2.1-T2V-1.3B",
            "prompt": "a lovely cat walking through tall grass",
            "video_frames": 33,
            "fps": 16
        })");
        s.examples = {example};
        s.errors = {kBodyNotJson, kArgMissing, kArgInvalid, kUnexpectedException};
        s.error_format = ErrorFormat::OpenAi;
        s.request_format = RequestFormat::Json;
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        try {
            const std::string model =
                canonical_model_name(ctx_, req.arg(model_loading::kModel).get<std::string>());
            req.body[model_loading::kModel.name] = model;
            // Set here, never taken from the client, and named before the job runs so
            // the clean-up below cannot remove a clip that is still being written.
            req.body["clip_file"] = video_store::new_file_name();
            remove_unreferenced_clips();

            lemon::jobs::StepRecord step;
            step.id = "video";
            step.op = "generate_video";
            step.params = lemon::jobs::json::parse(req.body.dump());
            std::vector<lemon::jobs::StepRecord> steps;
            steps.push_back(std::move(step));
            const std::string id =
                ctx_.job_manager->create("video", std::move(steps), lemon::jobs::json::object());

            const auto job = ctx_.job_manager->get(id);
            json body = {{"id", id},
                         {"status", job ? job->value("status", "queued") : "queued"},
                         {"model", model}};
            if (job && job->contains("created_at")) body["created_at"] = (*job)["created_at"];
            res.status = 202;
            res.set_content(body.dump(), "application/json");
        } catch (const std::exception& e) {
            LOG(ERROR, "Server") << "ERROR in videos_create: " << e.what() << std::endl;
            write_openai_error(res, kUnexpectedException, e.what(), "internal_error");
        }
    }

private:
    void remove_unreferenced_clips() {
        std::set<std::string> keep;
        for (const auto& summary : ctx_.job_manager->list()) {
            const auto job = ctx_.job_manager->get(summary.value("id", ""));
            if (!job || !job->contains("steps") || !(*job)["steps"].is_array()) continue;
            for (const auto& step : (*job)["steps"]) {
                for (const char* where : {"params", "output"}) {
                    if (step.contains(where) && step[where].is_object() &&
                        step[where].contains("clip_file") && step[where]["clip_file"].is_string())
                        keep.insert(step[where]["clip_file"].get<std::string>());
                }
            }
        }
        video_store::remove_all_except(keep);
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_videos_create_route(ServerContext& ctx) {
    return std::make_unique<VideosCreateRoute>(ctx);
}

} // namespace lemon
