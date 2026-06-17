#pragma once

#include <httplib.h>

namespace lemon {

class RequestLogService;

void handle_request_log_recent(RequestLogService* service,
                               const httplib::Request& req,
                               httplib::Response& res);

void handle_request_log_search(RequestLogService* service,
                               const httplib::Request& req,
                               httplib::Response& res);

void handle_request_log_stats(RequestLogService* service,
                              const httplib::Request& req,
                              httplib::Response& res);

} // namespace lemon
