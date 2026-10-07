#pragma once

#include <atomic>
#include <string>

#include <httplib.h>

namespace lemon {

class RouteRegistry;
class RuntimeConfig;

// Every request passes the same checks before any route sees it: origin and CORS,
// the API and admin keys, and the telemetry and session context the inference path
// reads. Doing them in one pre-routing function means no route can skip them.
class RequestMiddleware {
public:
    RequestMiddleware(RuntimeConfig* config, const RouteRegistry* registry);

    // Installs the pre-routing function, CORS headers and preflight answer, the error
    // handler, and the access log on one server.
    void install(httplib::Server& server);

    bool api_key_set() const { return !api_key_.empty(); }
    bool admin_api_key_set() const { return !admin_api_key_.empty(); }

private:
    httplib::Server::HandlerResponse pre_route(const httplib::Request& req,
                                               httplib::Response& res);
    httplib::Server::HandlerResponse authenticate(const httplib::Request& req,
                                                  httplib::Response& res);
    void log_response(const httplib::Request& req, const httplib::Response& res);

    RuntimeConfig* config_;
    const RouteRegistry* registry_;
    std::string api_key_;
    std::string admin_api_key_;
    std::atomic<bool> metrics_access_logged_{false};
};

} // namespace lemon
