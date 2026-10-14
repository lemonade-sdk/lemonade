#include "lemon/backends/configured/backend_transport.h"

namespace lemon::backends {

json BackendTransport::send_json(const MappedRequest& request) const {
    if (request.operation->kind != "http_json" || request.operation->codec != "json.v1" || !access_.post_json) throw std::invalid_argument("No JSON transport adapter");
    return access_.post_json(request.operation->path, request.body, request.operation->timeout_seconds);
}
void BackendTransport::send_stream(const MappedRequest& request, httplib::DataSink& sink, WrappedServer::TelemetryCallback telemetry) const {
    const bool sse = request.operation->kind == "http_sse" && request.operation->codec == "openai_sse.v1";
    const bool bytes = request.operation->kind == "http_bytes" && request.operation->codec == "raw_bytes.v1";
    if ((!sse && !bytes) || !access_.post_stream) throw std::invalid_argument("No sink transport adapter");
    access_.post_stream(request.operation->path, request.original_body ? *request.original_body : request.body.dump(), sink, sse, request.operation->timeout_seconds, std::move(telemetry));
}

}
