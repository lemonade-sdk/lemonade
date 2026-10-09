#pragma once

#include <httplib.h>

#include "lemon/server/server_context.h"

namespace lemon {

// Serves the browser UI from the same port as the API: the React web app (or the
// legacy status page when the web app is not built), its static assets, and the SPA
// fallback that lets client-side routes survive a reload.
class WebUi {
public:
    explicit WebUi(ServerContext& ctx) : ctx_(ctx) {}

    // The SPA fallback matches any GET path, so this registers after every API route.
    void register_routes(httplib::Server& server);

private:
    ServerContext& ctx_;
};

} // namespace lemon
