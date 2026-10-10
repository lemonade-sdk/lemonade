#pragma once
#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace lemon::telemetry {

struct ToolCall {
    std::string id;
    std::string function_name;
    std::string function_arguments;
};

// Start the metrics worker explicitly after process-wide startup configuration.
void initialize();

class InferenceSpan {
public:
    InferenceSpan(const std::string& span_kind, const std::string& name, const std::string& model_name, const nlohmann::json& request_json);
    ~InferenceSpan(); // Auto-completes with error if not ended

    void set_attribute(const std::string& key, const nlohmann::json& value);
    void end_with_success(const nlohmann::json& usage_or_timings, const std::string& complete_output, const std::vector<ToolCall>& tool_calls = {});
    void end_with_error(const std::string& error_message);
    void cancel();



private:
    std::string span_kind_;
    std::string name_;
    std::string model_name_;
    std::string request_dump_;
    std::string trace_id_;
    std::string span_id_;
    std::string parent_span_id_;
    std::string user_id_;
    std::string session_id_;
    std::chrono::steady_clock::time_point start_time_;
    bool ended_ = false;

    struct Message {
        std::string role;
        std::string content;
    };
    std::vector<Message> input_messages_;
    std::map<std::string, nlohmann::json> custom_attributes_;

    nlohmann::json build_common_attributes(bool has_openinference, bool has_otel_genai, bool hide_inputs);
    void submit_span(const nlohmann::json& span_details);
};

class TelemetryTracker {
public:
    static std::shared_ptr<InferenceSpan> start_span(const std::string& span_kind, const std::string& name, const std::string& model_name, const nlohmann::json& request_json);
};

void shutdown();
void flush();

void end_llm_span_async(
    std::shared_ptr<InferenceSpan> span,
    const std::string& metrics_url,
    std::function<std::map<std::string, nlohmann::json>(const std::string&)> parser,
    const nlohmann::json& usage_payload,
    const std::string& text_output,
    const std::vector<ToolCall>& tool_calls = {});

using SpanListenerCallback = std::function<void(const nlohmann::json&)>;
void register_span_listener(SpanListenerCallback callback);
void unregister_span_listener();
bool has_span_listeners();
void emit_span(const nlohmann::json& span_details);
std::string hash_token(const std::string& token);

extern thread_local std::string g_current_auth_token;
extern thread_local std::chrono::steady_clock::time_point g_request_start_time;
extern thread_local std::string g_current_client_session_id;
extern thread_local std::string g_incoming_trace_id;
extern thread_local std::string g_incoming_parent_span_id;
extern thread_local std::string g_incoming_client_id;
extern thread_local std::string g_incoming_session_id;

// The routing decision for the current request, as a compact JSON object
// ({collection, to, matched_rule, default_used} plus the flattened
// estimated_cost fields). Empty when the request did not go through a
// collection.router. A thread-local because the inference span is created deep
// in Router, where the Decision is no longer in scope; cleared per request like
// the trace-context fields above so it cannot leak across a reused worker.
extern thread_local std::string g_current_route_decision;

// Map a g_current_route_decision payload to its span attributes, under the
// existing llm.* namespace. Split out from apply_route_attributes so the
// mapping is unit-testable without constructing a span. An empty, malformed,
// or non-object payload yields no attributes, and a non-scalar member is
// skipped rather than stringified -- a span attribute is a scalar.
std::map<std::string, nlohmann::json> route_span_attributes(const std::string& payload);

// Mirror the current request's routing decision onto its inference span, so a
// trace shows which candidate a collection.router picked and what that
// candidate was reported to cost. No-op when the request was not routed.
void apply_route_attributes(InferenceSpan& span);

} // namespace lemon::telemetry
