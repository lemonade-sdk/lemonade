#pragma once

#include <memory>
#include <set>
#include <string>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "lemon/server/api_route.h"

namespace lemon {

// True for a route policy family name: [a-z0-9][a-z0-9_.-]{0,62}. The Nexus SDK
// matches the same grammar for the `svc:<family>` permission names on the tree.
bool valid_route_family(const std::string& name);

// Owns every route and turns each RouteSpec into httplib registrations, so URL
// prefixes, 405 stubs and registration order are decided in one place.
class RouteRegistry {
public:
    // Calls route->spec() once and keeps the result for every request. Throws when the
    // spec is malformed, so a bad route fails at startup. `family` is the route policy
    // family the route belongs to (RouteSpec::family); every route needs one, because
    // the mesh route catalog is built from the whole registry.
    void add(std::unique_ptr<ApiRoute> route, const std::string& family);

    // Registers every route in add() order. httplib serves the first registered match, so
    // a route folder adds a specific path before a pattern that also matches it.
    void apply(httplib::Server& server) const;

    // Registers every route except the /internal/ control routes. For a listener that
    // other machines reach, such as the Nexus mesh ingress: a peer gets the API but can
    // never stop lemond, change its config or remove its model files.
    void apply_without_internal(httplib::Server& server) const;

    // Every RouteSpec, in registration order.
    nlohmann::ordered_json list() const;

    // RequestMiddleware leaves these exact paths out of the access log.
    bool is_quiet(const std::string& path) const;

    // Runs the route that would serve req, without a socket. Returns false when none matches.
    bool dispatch(const httplib::Request& req, httplib::Response& res) const;

private:
    void apply(httplib::Server& server, bool include_internal) const;

    struct Entry {
        std::unique_ptr<ApiRoute> route;
        RouteSpec spec;
        std::vector<std::string> patterns;  // every prefixed URL, as an httplib regex
    };

    std::vector<std::shared_ptr<Entry>> entries_;
    std::set<std::string> quiet_paths_;
};

} // namespace lemon
