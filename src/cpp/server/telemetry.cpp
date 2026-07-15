#include "telemetry.h"
#include <mbedtls/md.h>
#include "lemon/runtime_config.h"
#include "lemon/utils/aixlog.hpp"
#include "lemon/utils/http_client.h"
#include "lemon/version.h"
#include <cctype>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <iomanip>
#include <mutex>
#include <queue>
#include <random>
#include <sstream>
#include <thread>
#include <vector>

namespace lemon::telemetry {

namespace {

class ProtoWriter {
public:
    std::string buf;

    void write_varint(uint64_t val) {
        while (val >= 0x80) {
            buf.push_back(static_cast<char>((val & 0x7F) | 0x80));
            val >>= 7;
        }
        buf.push_back(static_cast<char>(val & 0x7F));
    }

    void write_key(uint32_t field_num, uint32_t wire_type) {
        write_varint((field_num << 3) | wire_type);
    }

    void write_int64(uint32_t field_num, int64_t val) {
        write_key(field_num, 0);
        write_varint(static_cast<uint64_t>(val));
    }

    void write_uint32(uint32_t field_num, uint32_t val) {
        write_key(field_num, 0);
        write_varint(val);
    }

    void write_bool(uint32_t field_num, bool val) {
        write_key(field_num, 0);
        buf.push_back(val ? 1 : 0);
    }

    void write_fixed64(uint32_t field_num, uint64_t val) {
        write_key(field_num, 1);
        for (int i = 0; i < 8; ++i) {
            buf.push_back(static_cast<char>((val >> (i * 8)) & 0xFF));
        }
    }

    void write_string(uint32_t field_num, const std::string& val) {
        write_key(field_num, 2);
        write_varint(val.size());
        buf.append(val);
    }

    void write_bytes(uint32_t field_num, const std::string& val) {
        write_key(field_num, 2);
        write_varint(val.size());
        buf.append(val);
    }

    void write_message(uint32_t field_num, const ProtoWriter& sub) {
        write_key(field_num, 2);
        write_varint(sub.buf.size());
        buf.append(sub.buf);
    }
};

} // namespace

inline uint8_t hex_char_to_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

std::string hex_to_bytes(const std::string& hex) {
    if (hex.length() % 2 != 0) {
        throw std::invalid_argument("hex string must have an even length");
    }
    std::string bytes;
    bytes.reserve(hex.length() / 2);
    for (size_t i = 0; i < hex.length(); i += 2) {
        uint8_t high = hex_char_to_val(hex[i]);
        uint8_t low = hex_char_to_val(hex[i + 1]);
        bytes.push_back(static_cast<char>((high << 4) | low));
    }
    return bytes;
}

std::string strip_thinking(const std::string& text) {
    std::string result = text;
    std::vector<std::pair<std::string, std::string>> tag_pairs = {
        {"<|think|>", "</|think|>"},
        {"<|think|>", "<turn|>"},
        {"<|think|>", "<|turn>"},
        {"<think>", "</think>"},
        {"<thought>", "</thought>"}
    };
    for (const auto& [start_tag, end_tag] : tag_pairs) {
        size_t start_pos = 0;
        while ((start_pos = result.find(start_tag, start_pos)) != std::string::npos) {
            size_t end_pos = result.find(end_tag, start_pos + start_tag.length());
            if (end_pos != std::string::npos) {
                size_t erase_len = end_pos - start_pos;
                if (end_tag == "</|think|>" || end_tag == "</think>" || end_tag == "</thought>") {
                    erase_len += end_tag.length();
                }
                result.erase(start_pos, erase_len);
            } else {
                result.erase(start_pos);
                break;
            }
        }
    }
    size_t first = result.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    size_t last = result.find_last_not_of(" \t\r\n");
    return result.substr(first, last - first + 1);
}

std::string standardize_thinking(const std::string& text) {
    std::string result = text;
    std::vector<std::string> start_tags = {"<|think|>", "<thought>"};
    for (const auto& tag : start_tags) {
        size_t pos = 0;
        while ((pos = result.find(tag, pos)) != std::string::npos) {
            result.replace(pos, tag.length(), "<think>");
            pos += 7;
        }
    }

    std::vector<std::string> end_tags = {"</|think|>", "</think|>", "</thought>"};
    for (const auto& tag : end_tags) {
        size_t pos = 0;
        while ((pos = result.find(tag, pos)) != std::string::npos) {
            result.replace(pos, tag.length(), "</think>");
            pos += 8;
        }
    }

    std::vector<std::string> transition_tags = {"<turn|>", "<|turn>"};
    for (const auto& transition_tag : transition_tags) {
        size_t search_pos = 0;
        while (true) {
            size_t think_pos = result.find("<think>", search_pos);
            if (think_pos == std::string::npos) {
                break;
            }
            size_t next_close = result.find("</think>", think_pos + 7);
            size_t transition_pos = result.find(transition_tag, think_pos + 7);

            if (transition_pos != std::string::npos && (next_close == std::string::npos || next_close > transition_pos)) {
                result.insert(transition_pos, "</think>\n");
                search_pos = transition_pos + 9 + transition_tag.length();
            } else {
                search_pos = (next_close != std::string::npos) ? next_close + 8 : std::string::npos;
                if (search_pos == std::string::npos) {
                    break;
                }
            }
        }
    }

    size_t search_start = 0;
    while (true) {
        size_t first_think = result.find("<think>", search_start);
        if (first_think == std::string::npos) {
            break;
        }
        size_t next_pos = first_think + 7;
        while (next_pos < result.size() && (result[next_pos] == ' ' || result[next_pos] == '\t' || result[next_pos] == '\r' || result[next_pos] == '\n')) {
            next_pos++;
        }
        if (next_pos + 7 <= result.size() && result.substr(next_pos, 7) == "<think>") {
            result.erase(first_think + 7, (next_pos + 7) - (first_think + 7));
            search_start = first_think;
        } else {
            search_start = first_think + 7;
        }
    }

    size_t search_end = 0;
    while (true) {
        size_t first_close = result.find("</think>", search_end);
        if (first_close == std::string::npos) {
            break;
        }
        size_t next_pos = first_close + 8;
        while (next_pos < result.size() && (result[next_pos] == ' ' || result[next_pos] == '\t' || result[next_pos] == '\r' || result[next_pos] == '\n')) {
            next_pos++;
        }
        if (next_pos + 8 <= result.size() && result.substr(next_pos, 8) == "</think>") {
            result.erase(first_close + 8, (next_pos + 8) - (first_close + 8));
            search_end = first_close;
        } else {
            search_end = first_close + 8;
        }
    }

    return result;
}

static std::string serialize_json_batch(const std::vector<nlohmann::json>& spans) {
    nlohmann::json otlp_payload = {
        {"resourceSpans", nlohmann::json::array({
            {
                {"resource", {
                    {"attributes", nlohmann::json::array({
                        {{"key", "service.name"}, {"value", {{"stringValue", "lemonade-server"}}}},
                        {{"key", "service.version"}, {"value", {{"stringValue", LEMON_VERSION_STRING}}}}
                    })}
                }},
                {"scopeSpans", nlohmann::json::array({
                    {
                        {"scope", {{"name", "lemonade-server"}, {"version", LEMON_VERSION_STRING}}},
                        {"spans", nlohmann::json::array()}
                    }
                })}
            }
        })}
    };

    auto& spans_arr = otlp_payload["resourceSpans"][0]["scopeSpans"][0]["spans"];
    for (const auto& span : spans) {
        spans_arr.push_back(span);
    }
    return otlp_payload.dump();
}

static std::string serialize_protobuf_batch(const std::vector<nlohmann::json>& spans) {
    ProtoWriter scope_spans_msg;

    ProtoWriter scope_msg;
    scope_msg.write_string(1, "lemonade-server");
    scope_msg.write_string(2, LEMON_VERSION_STRING);
    scope_spans_msg.write_message(1, scope_msg);

    for (const auto& span_details : spans) {
        ProtoWriter span_msg;

        std::string trace_id_hex = span_details["traceId"].get<std::string>();
        span_msg.write_bytes(1, hex_to_bytes(trace_id_hex));

        std::string span_id_hex = span_details["spanId"].get<std::string>();
        span_msg.write_bytes(2, hex_to_bytes(span_id_hex));

        span_msg.write_string(5, span_details["name"].get<std::string>());

        span_msg.write_uint32(6, span_details["kind"].get<uint32_t>());

        uint64_t start_nano = std::stoull(span_details["startTimeUnixNano"].get<std::string>());
        span_msg.write_fixed64(7, start_nano);

        uint64_t end_nano = std::stoull(span_details["endTimeUnixNano"].get<std::string>());
        span_msg.write_fixed64(8, end_nano);

        if (span_details.contains("attributes") && span_details["attributes"].is_array()) {
            for (const auto& attr : span_details["attributes"]) {
                std::string key = attr["key"].get<std::string>();
                auto val_obj = attr["value"];
                ProtoWriter kv;
                kv.write_string(1, key);

                ProtoWriter any_val;
                if (val_obj.contains("stringValue")) {
                    any_val.write_string(1, val_obj["stringValue"].get<std::string>());
                } else if (val_obj.contains("intValue")) {
                    any_val.write_int64(3, val_obj["intValue"].get<int64_t>());
                } else if (val_obj.contains("boolValue")) {
                    any_val.write_bool(2, val_obj["boolValue"].get<bool>());
                } else if (val_obj.contains("doubleValue")) {
                    union { double d; uint64_t u; } u_val;
                    u_val.d = val_obj["doubleValue"].get<double>();
                    any_val.write_fixed64(4, u_val.u);
                }
                kv.write_message(2, any_val);
                span_msg.write_message(9, kv);
            }
        }

        if (span_details.contains("status")) {
            auto status_json = span_details["status"];
            ProtoWriter status_msg;
            if (status_json.contains("message")) {
                status_msg.write_string(2, status_json["message"].get<std::string>());
            }
            status_msg.write_uint32(3, status_json["code"].get<uint32_t>());
            span_msg.write_message(15, status_msg);
        }

        scope_spans_msg.write_message(2, span_msg);
    }

    ProtoWriter res_attr1;
    {
        ProtoWriter any_val;
        any_val.write_string(1, "lemonade-server");
        res_attr1.write_string(1, "service.name");
        res_attr1.write_message(2, any_val);
    }
    ProtoWriter res_attr2;
    {
        ProtoWriter any_val;
        any_val.write_string(1, LEMON_VERSION_STRING);
        res_attr2.write_string(1, "service.version");
        res_attr2.write_message(2, any_val);
    }

    ProtoWriter resource_msg;
    resource_msg.write_message(1, res_attr1);
    resource_msg.write_message(1, res_attr2);

    ProtoWriter resource_spans_msg;
    resource_spans_msg.write_message(1, resource_msg);
    resource_spans_msg.write_message(2, scope_spans_msg);

    ProtoWriter request_msg;
    request_msg.write_message(1, resource_spans_msg);

    return request_msg.buf;
}

class MetricsWorker {
public:
    MetricsWorker() : shutdown_(false), processing_(false) {
        worker_thread_ = std::thread(&MetricsWorker::run, this);
    }

    ~MetricsWorker() {
        stop();
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (shutdown_) return;
            shutdown_ = true;
        }
        cv_.notify_all();
        cv_drain_.notify_all();
        if (worker_thread_.joinable()) {
            worker_thread_.join();
        }
    }

    bool enqueue(std::shared_ptr<InferenceSpan> span,
                 const std::string& url,
                 std::function<std::map<std::string, nlohmann::json>(const std::string&)> parser,
                 const nlohmann::json& usage_payload,
                 const std::string& text_output) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.size() >= 100) {
            return false;
        }
        queue_.push({span, url, parser, usage_payload, text_output});
        cv_.notify_one();
        return true;
    }

    void drain() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_drain_.wait(lock, [this]() { return queue_.empty() && !processing_; });
    }

private:
    struct Task {
        std::shared_ptr<InferenceSpan> span;
        std::string metrics_url;
        std::function<std::map<std::string, nlohmann::json>(const std::string&)> parser;
        nlohmann::json usage_payload;
        std::string text_output;
    };

    std::queue<Task> queue_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::condition_variable cv_drain_;
    bool shutdown_ = false;
    bool processing_ = false;
    std::thread worker_thread_;

    void run() {
        while (true) {
            Task task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this]() { return shutdown_ || !queue_.empty(); });
                if (shutdown_ && queue_.empty()) {
                    break;
                }
                task = std::move(queue_.front());
                queue_.pop();
                processing_ = true;
            }

            if (!task.metrics_url.empty() && task.parser) {
                try {
                    auto response = utils::HttpClient::get(
                        task.metrics_url, {}, 1, utils::HttpSecurityPolicy::TrustedLoopback);
                    if (response.status_code == 200) {
                        auto extra = task.parser(response.body);
                        for (const auto& [k, v] : extra) {
                            task.span->set_attribute(k, v);
                        }
                    }
                } catch (const std::exception& e) {
                    LOG(DEBUG, "Telemetry") << "Failed to fetch metrics in background: " << e.what() << std::endl;
                } catch (...) {
                    LOG(DEBUG, "Telemetry") << "Failed to fetch metrics in background: unknown error" << std::endl;
                }
            }
            task.span->end_with_success(task.usage_payload, task.text_output);

            {
                std::lock_guard<std::mutex> lock(mutex_);
                processing_ = false;
                if (queue_.empty()) {
                    cv_drain_.notify_all();
                }
            }
        }
    }
};

static MetricsWorker g_metrics_worker;

class TelemetryQueue {
private:
    struct Task {
        nlohmann::json span_details;
        std::string endpoint;
        std::map<std::string, std::string> headers;
        std::string protocol;
        std::chrono::steady_clock::time_point arrival_time;
    };

    static constexpr size_t MAX_CAPACITY = 1000;
    std::deque<Task> queue_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;
    bool shutdown_ = false;
    size_t dropped_spans_count_ = 0;
    bool endpoint_unreachable_ = false;
    std::string last_endpoint_;
    bool last_enabled_ = false;
    bool flush_requested_ = false;
    std::condition_variable cv_flush_;

    void worker_loop() {
        while (true) {
            std::vector<nlohmann::json> batch_spans;
            std::string batch_endpoint;
            std::map<std::string, std::string> batch_headers;
            std::string batch_protocol;

            {
                std::unique_lock<std::mutex> lock(mutex_);

                while (true) {
                    if (shutdown_ && queue_.empty()) {
                        if (flush_requested_) {
                            flush_requested_ = false;
                            cv_flush_.notify_all();
                        }
                        return;
                    }
                    if (shutdown_ && !queue_.empty()) {
                        const auto& oldest_task = queue_.front();
                        batch_endpoint = oldest_task.endpoint;
                        batch_headers = oldest_task.headers;
                        batch_protocol = oldest_task.protocol;

                        auto it = queue_.begin();
                        while (it != queue_.end()) {
                            if (it->endpoint == batch_endpoint &&
                                it->headers == batch_headers &&
                                it->protocol == batch_protocol) {
                                batch_spans.push_back(std::move(it->span_details));
                                it = queue_.erase(it);
                            } else {
                                ++it;
                            }
                        }
                        break;
                    }
                    if (flush_requested_) {
                        if (queue_.empty()) {
                            flush_requested_ = false;
                            cv_flush_.notify_all();
                            break;
                        }
                        const auto& oldest_task = queue_.front();
                        batch_endpoint = oldest_task.endpoint;
                        batch_headers = oldest_task.headers;
                        batch_protocol = oldest_task.protocol;

                        int batch_size = 100;
                        if (auto* config = RuntimeConfig::global()) {
                            batch_size = config->telemetry_otlp_send_batch_size();
                        }

                        auto it = queue_.begin();
                        while (it != queue_.end() && static_cast<int>(batch_spans.size()) < batch_size) {
                            if (it->endpoint == batch_endpoint &&
                                it->headers == batch_headers &&
                                it->protocol == batch_protocol) {
                                batch_spans.push_back(std::move(it->span_details));
                                it = queue_.erase(it);
                            } else {
                                ++it;
                            }
                        }
                        LOG(DEBUG, "Telemetry") << "Flush requested. Exporting batch of "
                                                << batch_spans.size() << " spans..." << std::endl;
                        break;
                    }
                    if (queue_.empty()) {
                        cv_.wait(lock);
                        continue;
                    }

                    const auto& oldest_task = queue_.front();
                    std::string target_endpoint = oldest_task.endpoint;
                    std::map<std::string, std::string> target_headers = oldest_task.headers;
                    std::string target_protocol = oldest_task.protocol;
                    auto oldest_arrival = oldest_task.arrival_time;

                    int batch_size = 100;
                    double timeout_s = 1.0;
                    if (auto* config = RuntimeConfig::global()) {
                        batch_size = config->telemetry_otlp_send_batch_size();
                        timeout_s = config->telemetry_otlp_batch_timeout_s();
                    }

                    int matching_count = 0;
                    for (const auto& task : queue_) {
                        if (task.endpoint == target_endpoint &&
                            task.headers == target_headers &&
                            task.protocol == target_protocol) {
                            matching_count++;
                        }
                    }

                    auto now = std::chrono::steady_clock::now();
                    double elapsed_s = std::chrono::duration<double>(now - oldest_arrival).count();

                    if (matching_count >= batch_size || elapsed_s >= timeout_s) {
                        batch_endpoint = target_endpoint;
                        batch_headers = target_headers;
                        batch_protocol = target_protocol;

                        auto it = queue_.begin();
                        while (it != queue_.end() && static_cast<int>(batch_spans.size()) < batch_size) {
                            if (it->endpoint == target_endpoint &&
                                it->headers == target_headers &&
                                it->protocol == target_protocol) {
                                batch_spans.push_back(std::move(it->span_details));
                                it = queue_.erase(it);
                            } else {
                                ++it;
                            }
                        }

                        LOG(DEBUG, "Telemetry") << "Batch target size reached or timeout elapsed. Exporting batch of "
                                                << batch_spans.size() << " spans..." << std::endl;
                        break;
                    } else {
                        double remaining_s = timeout_s - elapsed_s;
                        if (remaining_s < 0) remaining_s = 0;
                        cv_.wait_for(lock, std::chrono::duration<double>(remaining_s));
                    }
                }
            }

            if (batch_spans.empty()) {
                continue;
            }

            std::string payload;
            if (batch_protocol == "http/json") {
                batch_headers["Content-Type"] = "application/json";
                payload = serialize_json_batch(batch_spans);
            } else {
                batch_headers["Content-Type"] = "application/x-protobuf";
                payload = serialize_protobuf_batch(batch_spans);
            }

            int max_retries = 0;
            if (auto* config = RuntimeConfig::global()) {
                max_retries = config->telemetry_otlp_max_retries();
            }

            bool bypass_retries = false;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                bypass_retries = endpoint_unreachable_;
            }
            int retries = 0;
            while (true) {
                bool success = false;
                bool retryable = true;
                std::string error_detail;
                try {
                    auto response = utils::HttpClient::post(batch_endpoint, payload, batch_headers, 3);
                    if (response.status_code >= 200 && response.status_code < 300) {
                        success = true;
                        LOG(DEBUG, "Telemetry") << "Successfully sent telemetry batch." << std::endl;
                        {
                            std::unique_lock<std::mutex> lock(mutex_);
                            endpoint_unreachable_ = false;
                        }
                    } else {
                        error_detail = "Status: " + std::to_string(response.status_code) + ", Response: " + response.body;
                        if (response.status_code >= 400 && response.status_code < 500 && response.status_code != 429) {
                            retryable = false;
                        }
                    }
                } catch (const std::exception& e) {
                    error_detail = e.what();
                } catch (...) {
                    error_detail = "Unknown exception";
                }

                if (success) {
                    break;
                }

                LOG(ERROR, "Telemetry") << "Failed to send telemetry batch. Telemetry receiver may be down or unreachable." << std::endl;
                LOG(DEBUG, "Telemetry") << "Telemetry batch failure details: " << error_detail << std::endl;

                if (!retryable) {
                    LOG(WARNING, "Telemetry") << "Telemetry batch dropped immediately due to non-retryable HTTP error." << std::endl;
                    break;
                }

                if (!bypass_retries && retries < max_retries) {
                    retries++;
                    double backoff_base = 5.0;
                    if (auto* config = RuntimeConfig::global()) {
                        backoff_base = config->telemetry_otlp_retry_backoff_base_s();
                    }
                    int shift = (std::min)(retries - 1, 10);
                    double delay = (std::min)(backoff_base * (1 << shift), 60.0);

                    thread_local std::mt19937 gen(std::random_device{}());
                    std::uniform_real_distribution<double> dist(0.5, 1.5);
                    double delay_with_jitter = delay * dist(gen);

                    LOG(DEBUG, "Telemetry") << "Retrying batch in " << delay_with_jitter << " seconds (with jitter, attempt " << retries << " of " << max_retries << ")..." << std::endl;

                    bool local_shutdown = false;
                    bool local_flush_requested = false;
                    {
                        std::unique_lock<std::mutex> lock(mutex_);
                        cv_.wait_for(lock, std::chrono::duration<double>(delay_with_jitter), [this]() { return shutdown_ || flush_requested_; });
                        local_shutdown = shutdown_;
                        local_flush_requested = flush_requested_;
                    }
                    if (local_shutdown) {
                        LOG(DEBUG, "Telemetry") << "Shutdown requested during retry sleep. Aborting." << std::endl;
                        return;
                    }
                    if (local_flush_requested) {
                        LOG(DEBUG, "Telemetry") << "Flush requested during retry sleep. Aborting retries for this batch." << std::endl;
                        break;
                    }
                } else {
                    {
                        std::unique_lock<std::mutex> lock(mutex_);
                        endpoint_unreachable_ = true;
                    }
                    if (max_retries > 0) {
                        LOG(WARNING, "Telemetry") << "Max retries reached (" << max_retries << ") or endpoint unreachable. Telemetry batch dropped." << std::endl;
                    } else {
                        LOG(WARNING, "Telemetry") << "Telemetry batch dropped (retries disabled)." << std::endl;
                    }
                    break;
                }
            }
        }
    }

public:
    TelemetryQueue() {
        worker_ = std::thread(&TelemetryQueue::worker_loop, this);
    }

    void flush() {
        std::unique_lock<std::mutex> lock(mutex_);
        if (queue_.empty()) {
            return;
        }
        flush_requested_ = true;
        cv_.notify_one();
        cv_flush_.wait(lock, [this]() { return !flush_requested_; });
    }

    void reset_unreachable() {
        std::unique_lock<std::mutex> lock(mutex_);
        endpoint_unreachable_ = false;
    }

    void shutdown() {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (shutdown_) return;
            shutdown_ = true;
        }
        cv_.notify_one();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    ~TelemetryQueue() {
        shutdown();
    }

    void push(nlohmann::json span_details, std::string endpoint, std::map<std::string, std::string> headers, std::string protocol) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (shutdown_) return;

        if (endpoint != last_endpoint_ || !last_enabled_) {
            last_endpoint_ = endpoint;
            last_enabled_ = true;
            endpoint_unreachable_ = false;
        }

        size_t max_capacity = MAX_CAPACITY;
        if (auto* config = RuntimeConfig::global()) {
            max_capacity = config->telemetry_max_queue_capacity();
        }

        if (queue_.size() >= max_capacity) {
            dropped_spans_count_++;
            if (dropped_spans_count_ % 100 == 1) {
                LOG(WARNING, "Telemetry") << "Telemetry queue full (capacity " << max_capacity
                                          << "). Dropped oldest span. Total dropped: " << dropped_spans_count_ << std::endl;
            }
            queue_.pop_front();
        }

        int batch_size = 100;
        if (auto* config = RuntimeConfig::global()) {
            batch_size = config->telemetry_otlp_send_batch_size();
        }
        LOG(DEBUG, "Telemetry") << "Accumulating span to batch (size " << (queue_.size() + 1) << "/" << batch_size << ")..." << std::endl;

        queue_.push_back({std::move(span_details), std::move(endpoint), std::move(headers), std::move(protocol), std::chrono::steady_clock::now()});
        cv_.notify_one();
    }
};

static TelemetryQueue& get_queue() {
    static TelemetryQueue queue;
    return queue;
}

void shutdown() {
    g_metrics_worker.stop();
    get_queue().shutdown();
}

void flush() {
    g_metrics_worker.drain();
    get_queue().flush();
}

static std::string generate_hex_id(size_t num_bytes) {
    thread_local std::mt19937 gen(std::random_device{}());
    std::uniform_int_distribution<unsigned short> dist(0, 255);
    std::stringstream ss;
    for (size_t i = 0; i < num_bytes; ++i) {
        ss << std::hex << std::setw(2) << std::setfill('0') << dist(gen);
    }
    return ss.str();
}

static uint64_t get_unix_nano() {
    auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
}

static std::string truncate_string(const std::string& str, size_t max_len) {
    if (str.length() <= max_len) {
        return str;
    }
    if (max_len <= 15) {
        return str.substr(0, max_len);
    }
    return str.substr(0, max_len - 15) + "... [TRUNCATED]";
}

InferenceSpan::InferenceSpan(const std::string& span_kind, const std::string& name, const std::string& model_name, const nlohmann::json& request_json)
    : span_kind_(span_kind), name_(name), model_name_(model_name), start_time_(std::chrono::steady_clock::now()) {
    trace_id_ = generate_hex_id(16);
    span_id_ = generate_hex_id(8);

    size_t max_len = 4096;
    if (auto* config = RuntimeConfig::global()) {
        max_len = static_cast<size_t>(config->telemetry_max_attribute_length());
    }

    if (request_json.contains("user") && request_json["user"].is_string()) {
        user_id_ = truncate_string(request_json["user"].get<std::string>(), max_len);
    }
    if (request_json.contains("session_id") && request_json["session_id"].is_string()) {
        session_id_ = truncate_string(request_json["session_id"].get<std::string>(), max_len);
    }

    if (span_kind_ == "LLM") {
        if (request_json.contains("messages") && request_json["messages"].is_array()) {
            request_dump_ = truncate_string(request_json["messages"].dump(), max_len);
            for (const auto& msg : request_json["messages"]) {
                if (msg.is_object()) {
                    Message message;
                    if (msg.contains("role") && msg["role"].is_string()) {
                        message.role = msg["role"].get<std::string>();
                    }
                    if (msg.contains("content") && msg["content"].is_string()) {
                        message.content = truncate_string(msg["content"].get<std::string>(), max_len);
                    } else if (msg.contains("content")) {
                        message.content = truncate_string(msg["content"].dump(), max_len);
                    }
                    input_messages_.push_back(message);
                }
            }
        } else if (request_json.contains("prompt") && request_json["prompt"].is_string()) {
            std::string prompt_str = request_json["prompt"].get<std::string>();
            request_dump_ = truncate_string(prompt_str, max_len);
            input_messages_.push_back({"user", request_dump_});
        } else {
            request_dump_ = truncate_string(request_json.dump(), max_len);
        }
    } else if (span_kind_ == "EMBEDDING") {
        if (request_json.contains("input")) {
            if (request_json["input"].is_string()) {
                request_dump_ = truncate_string(request_json["input"].get<std::string>(), max_len);
            } else {
                request_dump_ = truncate_string(request_json["input"].dump(), max_len);
            }
        } else {
            request_dump_ = truncate_string(request_json.dump(), max_len);
        }
    } else {
        request_dump_ = truncate_string(request_json.dump(), max_len);
    }

    if (g_request_start_time != std::chrono::steady_clock::time_point()) {
        auto queue_dur = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - g_request_start_time
        ).count();
        set_attribute("lemon.queue_time_ms", queue_dur);
    }
    if (!g_current_auth_token.empty()) {
        set_attribute("lemon.auth_token_hash", hash_token(g_current_auth_token));
    }
    if (!g_current_client_session_id.empty()) {
        set_attribute("lemon.client_session_id", g_current_client_session_id);
    }
}

InferenceSpan::~InferenceSpan() {
    if (!ended_) {
        end_with_error("Span destroyed before explicit completion");
    }
}

void InferenceSpan::set_attribute(const std::string& key, const nlohmann::json& value) {
    custom_attributes_[key] = value;
}

static void get_telemetry_semantics(bool& has_openinference, bool& has_otel_genai) {
    std::vector<std::string> semantics = {"openinference"};
    if (auto* config = RuntimeConfig::global()) {
        semantics = config->telemetry_otlp_semantics();
    }
    has_openinference = std::find(semantics.begin(), semantics.end(), "openinference") != semantics.end();
    has_otel_genai = std::find(semantics.begin(), semantics.end(), "otel_genai") != semantics.end();
}

nlohmann::json InferenceSpan::build_common_attributes(bool has_openinference, bool has_otel_genai, bool hide_inputs) {
    nlohmann::json attributes = nlohmann::json::array();
    std::string final_input = hide_inputs ? "[REDACTED]" : request_dump_;

    if (has_openinference) {
        attributes.push_back({{"key", "openinference.span.kind"}, {"value", {{"stringValue", span_kind_}}}});

        if (span_kind_ == "LLM") {
            attributes.push_back({{"key", "llm.model_name"}, {"value", {{"stringValue", model_name_}}}});
        } else if (span_kind_ == "EMBEDDING") {
            attributes.push_back({{"key", "embedding.model_name"}, {"value", {{"stringValue", model_name_}}}});
        } else if (span_kind_ == "RERANKER") {
            attributes.push_back({{"key", "reranker.model_name"}, {"value", {{"stringValue", model_name_}}}});
        }

        attributes.push_back({{"key", "input.value"}, {"value", {{"stringValue", final_input}}}});

        if (!user_id_.empty()) {
            attributes.push_back({{"key", "openinference.user.id"}, {"value", {{"stringValue", user_id_}}}});
        }
        if (!session_id_.empty()) {
            attributes.push_back({{"key", "openinference.session.id"}, {"value", {{"stringValue", session_id_}}}});
        }

        if (span_kind_ == "LLM") {
            for (size_t i = 0; i < input_messages_.size(); ++i) {
                std::string role_key = "llm.input_messages." + std::to_string(i) + ".message.role";
                std::string content_key = "llm.input_messages." + std::to_string(i) + ".message.content";
                attributes.push_back({{"key", role_key}, {"value", {{"stringValue", input_messages_[i].role}}}});
                attributes.push_back({{"key", content_key}, {"value", {{"stringValue", hide_inputs ? "[REDACTED]" : input_messages_[i].content}}}});
            }
        }
    }

    if (has_otel_genai) {
        std::string op_name = "chat";
        if (span_kind_ == "EMBEDDING") op_name = "embeddings";
        else if (span_kind_ == "RERANKER") op_name = "rerank";
        else if (name_ != "chat.completions") op_name = "completion";

        attributes.push_back({{"key", "gen_ai.operation.name"}, {"value", {{"stringValue", op_name}}}});
        attributes.push_back({{"key", "gen_ai.request.model"}, {"value", {{"stringValue", model_name_}}}});
        attributes.push_back({{"key", "gen_ai.provider.name"}, {"value", {{"stringValue", "lemonade"}}}});
        attributes.push_back({{"key", "gen_ai.system"}, {"value", {{"stringValue", "lemonade"}}}});

        if (!session_id_.empty()) {
            attributes.push_back({{"key", "gen_ai.conversation.id"}, {"value", {{"stringValue", session_id_}}}});
        }

        if (span_kind_ == "LLM") {
            for (size_t i = 0; i < input_messages_.size(); ++i) {
                std::string role_key = "gen_ai.input.messages." + std::to_string(i) + ".role";
                std::string content_key = "gen_ai.input.messages." + std::to_string(i) + ".content";
                attributes.push_back({{"key", role_key}, {"value", {{"stringValue", input_messages_[i].role}}}});
                attributes.push_back({{"key", content_key}, {"value", {{"stringValue", hide_inputs ? "[REDACTED]" : input_messages_[i].content}}}});
            }
        }
    }

    for (const auto& [k, v] : custom_attributes_) {
        if (v.is_string()) {
            attributes.push_back({{"key", k}, {"value", {{"stringValue", v.get<std::string>()}}}});
        } else if (v.is_number_integer()) {
            attributes.push_back({{"key", k}, {"value", {{"intValue", v.get<int64_t>()}}}});
        } else if (v.is_boolean()) {
            attributes.push_back({{"key", k}, {"value", {{"boolValue", v.get<bool>()}}}});
        } else if (v.is_number_float()) {
            attributes.push_back({{"key", k}, {"value", {{"doubleValue", v.get<double>()}}}});
        } else {
            attributes.push_back({{"key", k}, {"value", {{"stringValue", v.dump()}}}});
        }
    }

    return attributes;
}

void InferenceSpan::end_with_success(const nlohmann::json& usage_or_timings, const std::string& complete_output) {
    if (ended_) return;
    ended_ = true;

    auto end_time = get_unix_nano();
    uint64_t start_nano = end_time - std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start_time_).count();

    bool hide_inputs = false;
    bool hide_outputs = false;
    bool hide_thinking = false;
    if (auto* config = RuntimeConfig::global()) {
        hide_inputs = config->telemetry_hide_inputs();
        hide_outputs = config->telemetry_hide_outputs();
        hide_thinking = config->telemetry_hide_thinking();
    }

    std::string final_output;
    if (hide_outputs) {
        final_output = "[REDACTED]";
    } else {
        final_output = complete_output;
        if (hide_thinking) {
            final_output = strip_thinking(final_output);
        } else {
            final_output = standardize_thinking(final_output);
        }
    }

    bool has_openinference = false;
    bool has_otel_genai = false;
    get_telemetry_semantics(has_openinference, has_otel_genai);

    nlohmann::json attributes = build_common_attributes(has_openinference, has_otel_genai, hide_inputs);

    int prompt_tokens = -1;
    int completion_tokens = -1;
    int total_tokens = -1;

    std::string prefix = "llm";
    if (span_kind_ == "EMBEDDING") prefix = "embedding";
    else if (span_kind_ == "RERANKER") prefix = "reranker";

    if (usage_or_timings.contains("prompt_tokens") && usage_or_timings["prompt_tokens"].is_number()) {
        prompt_tokens = usage_or_timings["prompt_tokens"].get<int>();
    }
    if (usage_or_timings.contains("completion_tokens") && usage_or_timings["completion_tokens"].is_number()) {
        completion_tokens = usage_or_timings["completion_tokens"].get<int>();
    }
    if (usage_or_timings.contains("total_tokens") && usage_or_timings["total_tokens"].is_number()) {
        total_tokens = usage_or_timings["total_tokens"].get<int>();
    } else if (prompt_tokens >= 0 && completion_tokens >= 0) {
        total_tokens = prompt_tokens + completion_tokens;
    }

    if (has_openinference) {
        attributes.push_back({{"key", "output.value"}, {"value", {{"stringValue", final_output}}}});

        if (span_kind_ == "LLM" && !final_output.empty()) {
            attributes.push_back({{"key", "llm.output_messages.0.message.role"}, {"value", {{"stringValue", "assistant"}}}});
            attributes.push_back({{"key", "llm.output_messages.0.message.content"}, {"value", {{"stringValue", final_output}}}});
        }

        if (prompt_tokens >= 0) {
            attributes.push_back({{"key", prefix + ".usage.prompt_tokens"}, {"value", {{"intValue", prompt_tokens}}}});
        }
        if (completion_tokens >= 0) {
            attributes.push_back({{"key", prefix + ".usage.completion_tokens"}, {"value", {{"intValue", completion_tokens}}}});
        }
        if (total_tokens >= 0) {
            attributes.push_back({{"key", prefix + ".usage.total_tokens"}, {"value", {{"intValue", total_tokens}}}});
        }

        if (prompt_tokens >= 0) {
            attributes.push_back({{"key", "llm.token_count.prompt"}, {"value", {{"intValue", prompt_tokens}}}});
        }
        if (completion_tokens >= 0) {
            attributes.push_back({{"key", "llm.token_count.completion"}, {"value", {{"intValue", completion_tokens}}}});
        }
        if (total_tokens >= 0) {
            attributes.push_back({{"key", "llm.token_count.total"}, {"value", {{"intValue", total_tokens}}}});
        }
    }

    if (has_otel_genai) {
        if (prompt_tokens >= 0) {
            attributes.push_back({{"key", "gen_ai.usage.input_tokens"}, {"value", {{"intValue", prompt_tokens}}}});
        }
        if (completion_tokens >= 0) {
            attributes.push_back({{"key", "gen_ai.usage.output_tokens"}, {"value", {{"intValue", completion_tokens}}}});
        }
        if (span_kind_ == "LLM" && !final_output.empty()) {
            attributes.push_back({{"key", "gen_ai.output.messages.0.role"}, {"value", {{"stringValue", "assistant"}}}});
            attributes.push_back({{"key", "gen_ai.output.messages.0.content"}, {"value", {{"stringValue", final_output}}}});
        }
    }

    nlohmann::json span_json = {
        {"traceId", trace_id_},
        {"spanId", span_id_},
        {"name", name_},
        {"kind", 2},
        {"startTimeUnixNano", std::to_string(start_nano)},
        {"endTimeUnixNano", std::to_string(end_time)},
        {"attributes", attributes},
        {"status", {{"code", 1}}}
    };

    submit_span(span_json);
}

void InferenceSpan::end_with_error(const std::string& error_message) {
    if (ended_) return;
    ended_ = true;

    auto end_time = get_unix_nano();
    uint64_t start_nano = end_time - std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start_time_).count();

    bool hide_inputs = false;
    if (auto* config = RuntimeConfig::global()) {
        hide_inputs = config->telemetry_hide_inputs();
    }

    bool has_openinference = false;
    bool has_otel_genai = false;
    get_telemetry_semantics(has_openinference, has_otel_genai);

    nlohmann::json attributes = build_common_attributes(has_openinference, has_otel_genai, hide_inputs);

    nlohmann::json span_json = {
        {"traceId", trace_id_},
        {"spanId", span_id_},
        {"name", name_},
        {"kind", 2},
        {"startTimeUnixNano", std::to_string(start_nano)},
        {"endTimeUnixNano", std::to_string(end_time)},
        {"attributes", attributes},
        {"status", {{"code", 2}, {"message", error_message}}}
    };

    submit_span(span_json);
}

void InferenceSpan::cancel() {
    ended_ = true;
}

static bool is_valid_header_token(const std::string& str) {
    for (char c : str) {
        if (c == '\r' || c == '\n' || c == '\0') {
            return false;
        }
    }
    return true;
}

static std::string to_lowercase(std::string str) {
    std::transform(str.begin(), str.end(), str.begin(), [](unsigned char c) {
        return std::tolower(c);
    });
    return str;
}

void InferenceSpan::submit_span(const nlohmann::json& span_details) {
    emit_span(span_details);

    auto* config = RuntimeConfig::global();
    if (!config || !config->telemetry_enabled()) return;

    nlohmann::json scrubbed_span_details = span_details;
    if (scrubbed_span_details.contains("attributes") && scrubbed_span_details["attributes"].is_array()) {
        auto& attrs = scrubbed_span_details["attributes"];
        for (auto it = attrs.begin(); it != attrs.end(); ) {
            if (it->is_object() && it->value("key", "") == "lemon.auth_token_hash") {
                it = attrs.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::string endpoint = config->telemetry_otlp_endpoint();
    auto raw_config_headers = config->telemetry_otlp_headers();
    std::string protocol = config->telemetry_otlp_protocol();

    std::map<std::string, std::string> headers;

    auto sanitize_and_insert = [&headers](std::string k, std::string v) {
        size_t start = 0;
        while (start < k.size() && std::isspace(static_cast<unsigned char>(k[start]))) {
            start++;
        }
        size_t end = k.size();
        while (end > start && std::isspace(static_cast<unsigned char>(k[end - 1]))) {
            end--;
        }
        k = k.substr(start, end - start);

        start = 0;
        while (start < v.size() && std::isspace(static_cast<unsigned char>(v[start]))) {
            start++;
        }
        end = v.size();
        while (end > start && std::isspace(static_cast<unsigned char>(v[end - 1]))) {
            end--;
        }
        v = v.substr(start, end - start);

        if (!k.empty() && is_valid_header_token(k) && is_valid_header_token(v)) {
            std::string k_lower = to_lowercase(k);
            if (k_lower != "content-type" && k_lower != "content-length") {
                headers[k] = v;
            } else {
                LOG(WARNING, "Telemetry") << "Disallowed overriding well-known OTLP header: " << k << std::endl;
            }
        } else if (!k.empty()) {
            LOG(WARNING, "Telemetry") << "Rejected invalid OTLP header key or value containing CR, LF, or NUL." << std::endl;
        }
    };

    for (const auto& [k, v] : raw_config_headers) {
        sanitize_and_insert(k, v);
    }

    if (const char* env_headers = std::getenv("OTEL_EXPORTER_OTLP_HEADERS")) {
        std::string env_str(env_headers);
        std::stringstream ss(env_str);
        std::string item;
        while (std::getline(ss, item, ',')) {
            size_t eq = item.find('=');
            if (eq != std::string::npos) {
                std::string k = item.substr(0, eq);
                std::string v = item.substr(eq + 1);
                sanitize_and_insert(k, v);
            }
        }
    }

    get_queue().push(std::move(scrubbed_span_details), std::move(endpoint), std::move(headers), std::move(protocol));
}

std::shared_ptr<InferenceSpan> TelemetryTracker::start_span(const std::string& span_kind, const std::string& name, const std::string& model_name, const nlohmann::json& request_json) {
    auto* config = RuntimeConfig::global();
    bool otel_enabled = config && config->telemetry_enabled();
    if (otel_enabled || has_span_listeners()) {
        return std::make_shared<InferenceSpan>(span_kind, name, model_name, request_json);
    }
    return nullptr;
}

void end_llm_span_async(
    std::shared_ptr<InferenceSpan> span,
    const std::string& metrics_url,
    std::function<std::map<std::string, nlohmann::json>(const std::string&)> parser,
    const nlohmann::json& usage_payload,
    const std::string& text_output) {

    if (!span) return;

    if (metrics_url.empty() || !parser) {
        span->end_with_success(usage_payload, text_output);
        return;
    }

    if (!g_metrics_worker.enqueue(span, metrics_url, parser, usage_payload, text_output)) {
        LOG(WARNING, "Telemetry") << "MetricsWorker queue full. Dropping optional metrics and completing span immediately." << std::endl;
        span->end_with_success(usage_payload, text_output);
    }
}

namespace {
    std::mutex g_listeners_mutex;
    std::vector<SpanListenerCallback> g_span_listeners;
} // namespace

void register_span_listener(SpanListenerCallback callback) {
    std::lock_guard<std::mutex> lock(g_listeners_mutex);
    g_span_listeners.push_back(std::move(callback));
}

void unregister_span_listener() {
    std::lock_guard<std::mutex> lock(g_listeners_mutex);
    g_span_listeners.clear();
}

bool has_span_listeners() {
    std::lock_guard<std::mutex> lock(g_listeners_mutex);
    return !g_span_listeners.empty();
}

void emit_span(const nlohmann::json& span_details) {
    std::vector<SpanListenerCallback> active_listeners;
    {
        std::lock_guard<std::mutex> lock(g_listeners_mutex);
        active_listeners = g_span_listeners;
    }
    for (auto& cb : active_listeners) {
        try {
            cb(span_details);
        } catch (const std::exception& e) {
            LOG(WARNING, "Telemetry") << "Span listener failed: " << e.what() << std::endl;
        } catch (...) {
            LOG(WARNING, "Telemetry") << "Span listener failed with unknown error" << std::endl;
        }
    }
}

static std::string get_telemetry_salt() {
    static std::string salt;
    static std::once_flag salt_once;
    std::call_once(salt_once, []() {
        std::random_device rd;
        std::mt19937_64 gen(rd());
        std::uniform_int_distribution<uint64_t> dis;
        std::stringstream ss;
        ss << std::hex << dis(gen) << dis(gen) << dis(gen) << dis(gen);
        salt = ss.str();
    });
    return salt;
}

std::string hash_token(const std::string& token) {
    if (token.empty()) return "";
    const mbedtls_md_info_t* md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md_info) return "";

    std::string input = get_telemetry_salt() + token;
    unsigned char digest[32] = {};
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);

    bool success = false;
    if (mbedtls_md_setup(&ctx, md_info, 0) == 0) {
        if (mbedtls_md_starts(&ctx) == 0 &&
            mbedtls_md_update(&ctx, reinterpret_cast<const unsigned char*>(input.data()), input.size()) == 0 &&
            mbedtls_md_finish(&ctx, digest) == 0) {
            success = true;
        }
    }
    mbedtls_md_free(&ctx);

    if (!success) {
        return "";
    }

    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (unsigned char b : digest) {
        oss << std::setw(2) << static_cast<unsigned int>(b);
    }
    return oss.str();
}

thread_local std::string g_current_auth_token;
thread_local std::chrono::steady_clock::time_point g_request_start_time;
thread_local std::string g_current_client_session_id;

} // namespace lemon::telemetry
