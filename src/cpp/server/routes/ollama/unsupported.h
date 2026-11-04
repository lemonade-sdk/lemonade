#pragma once

#include <httplib.h>

#include "lemon/server/api_route.h"

namespace lemon {

// Ollama clients call model-management operations that Lemonade has no equivalent for,
// and each answers them the same way.
class OllamaUnsupportedRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    void handle(RouteRequest& req, httplib::Response& res) override;

    static const RouteError kNotSupported;
};

} // namespace lemon
