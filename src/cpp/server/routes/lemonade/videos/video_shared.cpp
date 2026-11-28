#include "video_shared.h"

#include "lemon/jobs/job_manager.h"
#include "lemon/server/api_route.h"
#include "lemon/server/server_context.h"
#include "lemon/server/video_store.h"

namespace lemon {

namespace video_routes {
const RouteArg kId{"id", ArgIn::Path, {{"type", "string"}}, true, Support::Available,
                   "Video id, from [`POST /v1/videos`](#post-v1videos)."};
} // namespace video_routes

const RouteError kUnknownVideo{404, "", "`id` names no video, or a deleted one"};
const RouteError kVideoNotActive{
    404, "", "`id` names no video, a deleted one, or one that is not queued or running"};
const RouteError kVideoNotReady{409, "", "the video's status is not yet `completed`"};
const RouteError kVideoNoOutput{500, "", "the video completed without an encoded clip"};

std::optional<nlohmann::json> find_video(ServerContext& ctx, const std::string& id) {
    const auto job = ctx.job_manager->get(id);
    if (!job) return std::nullopt;
    nlohmann::json record = job->to_api_json();
    if (!video_store::is_video_job(record)) return std::nullopt;
    return record;
}

void prune_video_files(ServerContext& ctx) {
    video_store::clean_up([&ctx] {
        video_store::CleanupSnapshot snapshot;
        for (const auto& summary : ctx.job_manager->list()) {
            const auto found = ctx.job_manager->get(summary.value("id", ""));
            if (!found) continue;
            snapshot.live_jobs.insert(found->id);
            for (const auto& step : found->steps) {
                for (const auto* record : {&step.params, &step.output}) {
                    if (record->is_object() && record->contains("clip_file") &&
                        (*record)["clip_file"].is_string())
                        snapshot.referenced.insert((*record)["clip_file"].get<std::string>());
                }
                if (step.op == "generate_video" && step.params.is_object() &&
                    step.params.contains("image_file") && step.params["image_file"].is_string())
                    snapshot.referenced.insert(step.params["image_file"].get<std::string>());
            }
        }
        return snapshot;
    });
}

void write_unknown_video(httplib::Response& res) {
    write_plain_error(res, kUnknownVideo, "unknown video");
}

} // namespace lemon
