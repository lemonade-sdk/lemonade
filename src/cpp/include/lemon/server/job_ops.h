#pragma once

#include "lemon/jobs/job_ops.h"
#include "lemon/server/server_context.h"

namespace lemon {

// The job engine runs server operations (load, unload, chat, models, system stats)
// without depending on Server, so it receives them as op providers built here.
struct JobOps {
    static lemon::jobs::OpProviders build(ServerContext& ctx);
};

} // namespace lemon
