#pragma once

#include "lemon/jobs/job_ops.h"
#include "lemon/server/server_context.h"

namespace lemon {

// The job engine runs system info/stats, models, sleep, load/unload, chat, image generation/edits/variations, speech/audio, and 3D
// without depending on Server, so it receives them as op providers built here.
struct JobOps {
    static lemon::jobs::OpProviders build(ServerContext& ctx);
};

} // namespace lemon
