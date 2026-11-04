#include "job_errors.h"

#include "lemon/server/api_route.h"

namespace lemon {

const RouteError kUnknownJob{404, "", "`id` names no job, or a deleted one"};
const RouteError kJobNotActive{
    404, "", "`id` names no job, a deleted one, or one that is not queued or running"};

void write_unknown_job(httplib::Response& res) {
    write_plain_error(res, kUnknownJob, "unknown job");
}

void write_job_not_active(httplib::Response& res, const std::string& refused_action) {
    write_plain_error(res, kJobNotActive, "job not found or not " + refused_action);
}

} // namespace lemon
