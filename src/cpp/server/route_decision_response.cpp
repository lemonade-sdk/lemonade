#include "lemon/route_decision_response.h"

#include <utility>

namespace lemon {
namespace {

struct EventBoundary {
    std::size_t pos = std::string::npos;
    std::size_t len = 0;
};

std::optional<EventBoundary> find_sse_event_boundary(const std::string& buffer) {
    for (std::size_t i = 0; i < buffer.size(); ++i) {
        if (buffer[i] != '\r' && buffer[i] != '\n') {
            continue;
        }

        std::size_t first_len = 1;
        if (buffer[i] == '\r') {
            if (i + 1 >= buffer.size()) {
                return std::nullopt;
            }
            if (buffer[i + 1] == '\n') {
                first_len = 2;
            }
        }

        const std::size_t second = i + first_len;
        if (second >= buffer.size()) {
            return std::nullopt;
        }
        if (buffer[second] == '\n') {
            return EventBoundary{i, first_len + 1};
        }
        if (buffer[second] == '\r') {
            if (second + 1 < buffer.size() && buffer[second + 1] == '\n') {
                return EventBoundary{i, first_len + 2};
            }
            if (first_len == 2 && second + 1 >= buffer.size()) {
                return std::nullopt;
            }
            return EventBoundary{i, first_len + 1};
        }
    }
    return std::nullopt;
}

} // namespace

json route_decision_to_json(const Decision& decision) {
    json out = {
        {"version", "1"},
        {"route_to", decision.route_to},
        {"matched_rule", decision.matched_rule},
        {"default_used", decision.default_used},
        {"outputs", decision.outputs.is_object() ? decision.outputs : json::object()},
    };
    if (!decision.trace.empty()) {
        out["trace"] = json::array();
        for (const auto& entry : decision.trace) {
            json trace_entry = {
                {"condition", entry.condition},
                {"result", entry.result},
            };
            if (entry.score.has_value()) {
                trace_entry["score"] = *entry.score;
            }
            if (!entry.label.empty()) {
                trace_entry["label"] = entry.label;
            }
            if (!entry.rationale.empty()) {
                trace_entry["rationale"] = entry.rationale;
            }
            out["trace"].push_back(std::move(trace_entry));
        }
    }
    return out;
}

std::string route_decision_header_value(const Decision& decision) {
    return decision.matched_rule.empty() ? "default" : decision.matched_rule;
}

void attach_route_header(httplib::Response& res, const Decision& decision) {
    res.set_header("x-lemonade-route", route_decision_header_value(decision));
}

std::string attach_route_decision_to_sse_event(
    const std::string& event,
    const json& route_decision_json,
    bool& attached) {
    if (attached || route_decision_json.is_null()) {
        return event;
    }

    const std::string prefix = "data:";
    std::size_t pos = event.find(prefix);
    if (pos == std::string::npos) {
        return event;
    }
    std::size_t payload_start = pos + prefix.size();
    while (payload_start < event.size() &&
           (event[payload_start] == ' ' || event[payload_start] == '\t')) {
        ++payload_start;
    }
    std::size_t payload_end = event.find('\n', payload_start);
    if (payload_end == std::string::npos) {
        payload_end = event.size();
    }
    while (payload_end > payload_start &&
           (event[payload_end - 1] == '\r' || event[payload_end - 1] == ' ' ||
            event[payload_end - 1] == '\t')) {
        --payload_end;
    }

    std::string payload = event.substr(payload_start, payload_end - payload_start);
    if (payload.empty() || payload == "[DONE]") {
        return event;
    }

    json parsed = json::parse(payload, nullptr, /*allow_exceptions=*/false);
    if (!parsed.is_object()) {
        return event;
    }
    parsed["x_lemonade_route"] = route_decision_json;
    attached = true;

    std::string out = event.substr(0, payload_start);
    out += parsed.dump();
    out += event.substr(payload_end);
    return out;
}

RouteDecisionSseSink::RouteDecisionSseSink(httplib::DataSink& inner,
                                           json route_decision_json)
    : inner_(inner), route_decision_json_(std::move(route_decision_json)) {}

bool RouteDecisionSseSink::write(const char* data, size_t length) {
    buffer_.append(data, length);
    while (true) {
        std::optional<EventBoundary> boundary = find_sse_event_boundary(buffer_);
        if (!boundary.has_value()) {
            break;
        }
        std::string event = buffer_.substr(0, boundary->pos + boundary->len);
        buffer_.erase(0, boundary->pos + boundary->len);
        event = attach_route_decision_to_sse_event(
            event, route_decision_json_, attached_);
        if (!inner_.write(event.c_str(), event.size())) {
            return false;
        }
    }
    return true;
}

void RouteDecisionSseSink::done() {
    flush();
    if (inner_.done) {
        inner_.done();
    }
}

void RouteDecisionSseSink::done_with_trailer(const httplib::Headers& trailer) {
    flush();
    if (inner_.done_with_trailer) {
        inner_.done_with_trailer(trailer);
    } else if (inner_.done) {
        inner_.done();
    }
}

bool RouteDecisionSseSink::is_writable() const {
    return !inner_.is_writable || inner_.is_writable();
}

void RouteDecisionSseSink::flush() {
    if (!buffer_.empty()) {
        std::string event = attach_route_decision_to_sse_event(
            buffer_, route_decision_json_, attached_);
        inner_.write(event.c_str(), event.size());
        buffer_.clear();
    }
}

} // namespace lemon
