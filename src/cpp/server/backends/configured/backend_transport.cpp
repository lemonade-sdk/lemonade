#include "lemon/backends/configured/backend_transport.h"

namespace lemon::backends {

json BackendTransport::send_json(const TransportRequest& request) const {
    const bool get = request.method == "GET";
    if (request.kind != "http_json" || request.codec != "json.v1" ||
        (!get && request.method != "POST") || !(get ? bool(access_.get_json) : bool(access_.post_json)))
        throw std::invalid_argument("No JSON transport adapter");
    return get ? access_.get_json(request.path, request.timeout_seconds) : access_.post_json(request.path, request.body, request.timeout_seconds);
}
void BackendTransport::send_stream(const TransportRequest& request, httplib::DataSink& sink, WrappedServer::TelemetryCallback telemetry) const {
    if (request.kind != "http_sse" || request.codec != "openai_sse.v1" || request.method != "POST" || !access_.post_stream)
        throw std::invalid_argument("No SSE transport adapter");
    access_.post_stream(request.path, request.original_body ? *request.original_body : request.body.dump(), sink, true, request.timeout_seconds, std::move(telemetry));
}

json BackendTransport::send_json(const MappedRequest& request) const {
    const bool get = request.operation->method == "GET";
    if (request.operation->kind != "http_json" || request.operation->codec != "json.v1" || !(get ? bool(access_.get_json) : bool(access_.post_json))) throw std::invalid_argument("No JSON transport adapter");
    return get ? access_.get_json(request.path, request.operation->timeout_seconds) : access_.post_json(request.path, request.body, request.operation->timeout_seconds);
}
void BackendTransport::send_stream(const MappedRequest& request, httplib::DataSink& sink, WrappedServer::TelemetryCallback telemetry) const {
    const bool sse = request.operation->kind == "http_sse" && request.operation->codec == "openai_sse.v1";
    const bool bytes = request.operation->kind == "http_bytes" && request.operation->codec == "raw_bytes.v1";
    if ((!sse && !bytes) || !access_.post_stream) throw std::invalid_argument("No sink transport adapter");
    access_.post_stream(request.path, request.original_body ? *request.original_body : request.body.dump(), sink, sse, request.operation->timeout_seconds, std::move(telemetry));
}

}
