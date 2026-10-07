#pragma once

#include <memory>
#include <set>
#include <string>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "lemon/server/api_route.h"

namespace lemon {

// Owns every route and turns each RouteSpec into httplib registrations, so URL
// prefixes, 405 stubs and registration order are decided in one place.
class RouteRegistry {
public:
    // Calls route->spec() once and keeps the result for every request. Throws when the
    // spec is malformed, so a bad route fails at startup.
    void add(std::unique_ptr<ApiRoute> route);

    // Registers every route in add() order. httplib serves the first registered match, so
    // a route folder adds a specific path before a pattern that also matches it.
    void apply(httplib::Server& server) const;

    // Every RouteSpec, in registration order, for GET /internal/routes.
    nlohmann::json list() const;

    // RequestMiddleware leaves these exact paths out of the access log.
    bool is_quiet(const std::string& path) const;

    // Runs the route that would serve req, without a socket. Returns false when none matches.
    bool dispatch(const httplib::Request& req, httplib::Response& res) const;

private:
    struct Entry {
        std::unique_ptr<ApiRoute> route;
        RouteSpec spec;
        std::vector<std::string> patterns;  // every prefixed URL, as an httplib regex
    };

    std::vector<std::shared_ptr<Entry>> entries_;
    std::set<std::string> quiet_paths_;
};

} // namespace lemon
