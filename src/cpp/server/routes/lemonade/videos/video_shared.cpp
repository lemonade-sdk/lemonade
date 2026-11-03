#include "video_shared.h"

#include "lemon/server/api_route.h"

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

void write_unknown_video(httplib::Response& res) {
    write_plain_error(res, kUnknownVideo, "unknown video");
}

} // namespace lemon
