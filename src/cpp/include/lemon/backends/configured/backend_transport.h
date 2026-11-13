#pragma once

#include "lemon/backends/configured/request_mapper.h"
#include "lemon/wrapped_server.h"

namespace lemon::backends {

struct TransportAccess {
    std::function<json(const std::string&, const json&, long)> post_json;
    std::function<json(const std::string&, long)> get_json;
    std::function<void(const std::string&, const std::string&, httplib::DataSink&, bool, long, WrappedServer::TelemetryCallback)> post_stream;
};
struct TransportRequest {
    std::string kind;
    std::string method;
    std::string codec;
    std::string path;
    json body;
    std::optional<std::string> original_body;
    long timeout_seconds = 0;
    bool sse = true;
};
class BackendTransport {
public:
    explicit BackendTransport(TransportAccess access) : access_(std::move(access)) {}
    json send_json(const MappedRequest& request) const;
    json send_json(const TransportRequest& request) const;
    void send_stream(const TransportRequest& request, httplib::DataSink& sink, WrappedServer::TelemetryCallback telemetry = nullptr) const;
    void send_stream(const MappedRequest& request, httplib::DataSink& sink, WrappedServer::TelemetryCallback telemetry = nullptr) const;
private:
    const TransportAccess access_;
};

}
