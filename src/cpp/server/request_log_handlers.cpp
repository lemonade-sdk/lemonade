#include "lemon/request_log_handlers.h"

#include "lemon/request_log_service.h"
#include "lemon/utils/aixlog.hpp"

#include <nlohmann/json.hpp>

namespace lemon {

namespace {

void set_service_unavailable(httplib::Response& res, const std::string& message) {
    res.status = 503;
    nlohmann::json error = {{"error", message}};
    res.set_content(error.dump(), "application/json");
}

} // namespace

void handle_request_log_recent(RequestLogService* service,
                               const httplib::Request& req,
                               httplib::Response& res) {
    if (req.method == "HEAD") {
        res.status = 200;
        return;
    }

    if (!service || !service->is_enabled()) {
        set_service_unavailable(res, "Request logging is not enabled");
        return;
    }

    int limit = 100;
    if (req.has_param("limit")) {
        try {
            limit = std::stoi(req.get_param_value("limit"));
            if (limit < 1) {
                limit = 1;
            }
            if (limit > 1000) {
                limit = 1000;
            }
        } catch (...) {
            res.status = 400;
            res.set_content(R"({"error":"Invalid limit parameter"})", "application/json");
            return;
        }
    }

    try {
        const auto payload = service->get_recent(limit);
        res.set_content(payload.dump(), "application/json");
    } catch (const std::exception& e) {
        LOG(ERROR, "RequestLog") << "handle_request_log_recent failed: " << e.what() << std::endl;
        set_service_unavailable(res, e.what());
    }
}

void handle_request_log_search(RequestLogService* service,
                               const httplib::Request& req,
                               httplib::Response& res) {
    if (req.method == "HEAD") {
        res.status = 200;
        return;
    }

    if (!service || !service->is_enabled()) {
        set_service_unavailable(res, "Request logging is not enabled");
        return;
    }

    try {
        const auto payload = service->search(req);
        res.set_content(payload.dump(), "application/json");
    } catch (const std::exception& e) {
        LOG(ERROR, "RequestLog") << "handle_request_log_search failed: " << e.what() << std::endl;
        set_service_unavailable(res, e.what());
    }
}

void handle_request_log_stats(RequestLogService* service,
                              const httplib::Request& req,
                              httplib::Response& res) {
    if (req.method == "HEAD") {
        res.status = 200;
        return;
    }

    if (!service || !service->is_enabled()) {
        set_service_unavailable(res, "Request logging is not enabled");
        return;
    }

    try {
        const auto payload = service->get_stats(req);
        res.set_content(payload.dump(), "application/json");
    } catch (const std::exception& e) {
        LOG(ERROR, "RequestLog") << "handle_request_log_stats failed: " << e.what() << std::endl;
        set_service_unavailable(res, e.what());
    }
}

} // namespace lemon
