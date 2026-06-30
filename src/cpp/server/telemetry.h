#pragma once
#include <string>
#include <map>
#include <chrono>
#include <memory>
#include <vector>
#include <nlohmann/json.hpp>

namespace lemon::telemetry {

class InferenceSpan {
public:
    InferenceSpan(const std::string& span_kind, const std::string& name, const std::string& model_name, const nlohmann::json& request_json);
    ~InferenceSpan(); // Auto-completes with error if not ended

    void set_attribute(const std::string& key, const nlohmann::json& value);
    void end_with_success(const nlohmann::json& usage_or_timings, const std::string& complete_output);
    void end_with_error(const std::string& error_message);
    void cancel();

    std::string trace_id() const { return trace_id_; }
    std::string span_id() const { return span_id_; }

private:
    std::string span_kind_;
    std::string name_;
    std::string model_name_;
    std::string request_dump_;
    std::string trace_id_;
    std::string span_id_;
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
    const std::string& text_output);

} // namespace lemon::telemetry
