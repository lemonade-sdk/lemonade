#pragma once

#include "routing_policy.h"

#include <httplib.h>

#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace lemon {

json route_decision_to_json(const Decision& decision);

std::string route_decision_header_value(const Decision& decision);

void attach_route_header(httplib::Response& res, const Decision& decision);

std::string attach_route_decision_to_sse_event(const std::string& event,
                                               const json& route_decision_json,
                                               bool& attached);

class RouteDecisionSseSink {
public:
    RouteDecisionSseSink(httplib::DataSink& inner, json route_decision_json);

    bool write(const char* data, size_t length);
    void done();
    void done_with_trailer(const httplib::Headers& trailer);
    bool is_writable() const;

private:
    void flush();

    httplib::DataSink& inner_;
    json route_decision_json_;
    bool attached_ = false;
    std::string buffer_;
};

template <typename StreamFn>
void stream_with_route_decision(httplib::DataSink& sink,
                                json route_decision_json,
                                StreamFn&& stream_fn) {
    if (route_decision_json.is_null()) {
        stream_fn(sink);
        return;
    }

    RouteDecisionSseSink route_sink_wrapper(sink, std::move(route_decision_json));
    httplib::DataSink route_sink;
    route_sink.write = [&route_sink_wrapper](const char* data, size_t len) {
        return route_sink_wrapper.write(data, len);
    };
    route_sink.done = [&route_sink_wrapper]() { route_sink_wrapper.done(); };
    route_sink.done_with_trailer = [&route_sink_wrapper](
        const httplib::Headers& trailer) {
        route_sink_wrapper.done_with_trailer(trailer);
    };
    route_sink.is_writable = [&route_sink_wrapper]() {
        return route_sink_wrapper.is_writable();
    };
    stream_fn(route_sink);
}

} // namespace lemon
