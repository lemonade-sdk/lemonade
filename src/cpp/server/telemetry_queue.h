#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include "lemon/runtime_config.h"
#include "lemon/utils/aixlog.hpp"
#include "lemon/utils/http_client.h"

namespace lemon::telemetry {

std::string serialize_json_batch(const std::vector<nlohmann::json>& spans);
std::string serialize_json_batch_strings(const std::vector<std::string>& spans);
std::string serialize_protobuf_batch(const std::vector<nlohmann::json>& spans);

class TelemetryQueue {
public:
    struct Task {
        std::string json_str;
        nlohmann::json span_details;
        std::string endpoint;
        std::map<std::string, std::string> headers;
        std::string protocol;
        std::chrono::steady_clock::time_point arrival_time;
        size_t approx_bytes = 0;
    };

    static constexpr size_t MAX_CAPACITY = 1000;
    static constexpr size_t MAX_BATCH_BYTES = 2097152;

private:
    std::deque<Task> queue_;
    size_t current_bytes_ = 0;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;
    bool shutdown_ = false;
    size_t dropped_spans_count_ = 0;
    bool endpoint_unreachable_ = false;
    std::string last_endpoint_;
    bool last_enabled_ = false;
    bool flush_requested_ = false;
    std::condition_variable cv_flush_;

    std::deque<Task>::iterator remove_task_at(std::deque<Task>::iterator it,
                                              std::vector<nlohmann::json>& batch_spans,
                                              std::vector<std::string>& batch_json_spans) {
        if (!it->json_str.empty()) {
            batch_json_spans.push_back(std::move(it->json_str));
        } else {
            batch_spans.push_back(std::move(it->span_details));
        }
        if (current_bytes_ >= it->approx_bytes) {
            current_bytes_ -= it->approx_bytes;
        } else {
            current_bytes_ = 0;
        }
        return queue_.erase(it);
    }

    void pop_front_task() {
        if (!queue_.empty()) {
            if (current_bytes_ >= queue_.front().approx_bytes) {
                current_bytes_ -= queue_.front().approx_bytes;
            } else {
                current_bytes_ = 0;
            }
            queue_.pop_front();
        }
    }

    void worker_loop() {
        while (true) {
            std::vector<nlohmann::json> batch_spans;
            std::vector<std::string> batch_json_spans;
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

                        size_t accumulated_batch_bytes = 0;
                        auto it = queue_.begin();
                        while (it != queue_.end()) {
                            if (it->endpoint == batch_endpoint &&
                                it->headers == batch_headers &&
                                it->protocol == batch_protocol) {
                                if ((!batch_spans.empty() || !batch_json_spans.empty()) &&
                                    accumulated_batch_bytes + it->approx_bytes > MAX_BATCH_BYTES) {
                                    break;
                                }
                                accumulated_batch_bytes += it->approx_bytes;
                                it = remove_task_at(it, batch_spans, batch_json_spans);
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

                        size_t accumulated_batch_bytes = 0;
                        auto it = queue_.begin();
                        while (it != queue_.end() && static_cast<int>(batch_spans.size() + batch_json_spans.size()) < batch_size) {
                            if (it->endpoint == batch_endpoint &&
                                it->headers == batch_headers &&
                                it->protocol == batch_protocol) {
                                if ((!batch_spans.empty() || !batch_json_spans.empty()) &&
                                    accumulated_batch_bytes + it->approx_bytes > MAX_BATCH_BYTES) {
                                    break;
                                }
                                accumulated_batch_bytes += it->approx_bytes;
                                it = remove_task_at(it, batch_spans, batch_json_spans);
                            } else {
                                ++it;
                            }
                        }
                        LOG(DEBUG, "Telemetry") << "Flush requested. Exporting batch of "
                                                << (batch_spans.size() + batch_json_spans.size()) << " spans..." << std::endl;
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
                    size_t matching_bytes = 0;
                    for (const auto& task : queue_) {
                        if (task.endpoint == target_endpoint &&
                            task.headers == target_headers &&
                            task.protocol == target_protocol) {
                            matching_count++;
                            matching_bytes += task.approx_bytes;
                        }
                    }

                    auto now = std::chrono::steady_clock::now();
                    double elapsed_s = std::chrono::duration<double>(now - oldest_arrival).count();

                    if (matching_count >= batch_size || matching_bytes >= MAX_BATCH_BYTES || elapsed_s >= timeout_s) {
                        batch_endpoint = target_endpoint;
                        batch_headers = target_headers;
                        batch_protocol = target_protocol;

                        size_t accumulated_batch_bytes = 0;
                        auto it = queue_.begin();
                        while (it != queue_.end() && static_cast<int>(batch_spans.size() + batch_json_spans.size()) < batch_size) {
                            if (it->endpoint == target_endpoint &&
                                it->headers == target_headers &&
                                it->protocol == target_protocol) {
                                if ((!batch_spans.empty() || !batch_json_spans.empty()) &&
                                    accumulated_batch_bytes + it->approx_bytes > MAX_BATCH_BYTES) {
                                    break;
                                }
                                accumulated_batch_bytes += it->approx_bytes;
                                it = remove_task_at(it, batch_spans, batch_json_spans);
                            } else {
                                ++it;
                            }
                        }

                        LOG(DEBUG, "Telemetry") << "Batch target size/byte limit reached or timeout elapsed. Exporting batch of "
                                                << (batch_spans.size() + batch_json_spans.size()) << " spans (" << accumulated_batch_bytes << " bytes)..." << std::endl;
                        break;
                    } else {
                        double remaining_s = timeout_s - elapsed_s;
                        if (remaining_s < 0) remaining_s = 0;
                        cv_.wait_for(lock, std::chrono::duration<double>(remaining_s));
                    }
                }
            }

            if (batch_spans.empty() && batch_json_spans.empty()) {
                continue;
            }

            std::string payload;
            if (batch_protocol == "http/json") {
                batch_headers["Content-Type"] = "application/json";
                if (!batch_json_spans.empty()) {
                    payload = serialize_json_batch_strings(batch_json_spans);
                } else {
                    payload = serialize_json_batch(batch_spans);
                }
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
                    const auto policy = batch_endpoint.rfind("http://", 0) == 0
                        ? utils::HttpSecurityPolicy::AllowInsecureHttp
                        : utils::HttpSecurityPolicy::ExternalHttpsOnly;
                    auto response = utils::HttpClient::post(
                        batch_endpoint,
                        payload,
                        batch_headers,
                        3,
                        policy);
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
        size_t task_bytes = endpoint.size() + protocol.size();
        for (const auto& [k, v] : headers) {
            task_bytes += k.size() + v.size();
        }

        std::string json_str;
        if (protocol == "http/json") {
            json_str = span_details.dump();
            task_bytes += json_str.size();
        } else {
            task_bytes += 128;
            if (span_details.contains("name") && span_details["name"].is_string()) {
                task_bytes += span_details["name"].get<std::string>().size();
            }
            if (span_details.contains("traceId") && span_details["traceId"].is_string()) {
                task_bytes += span_details["traceId"].get<std::string>().size();
            }
            if (span_details.contains("spanId") && span_details["spanId"].is_string()) {
                task_bytes += span_details["spanId"].get<std::string>().size();
            }
            if (span_details.contains("parentSpanId") && span_details["parentSpanId"].is_string()) {
                task_bytes += span_details["parentSpanId"].get<std::string>().size();
            }
            if (span_details.contains("status") && span_details["status"].is_object() &&
                span_details["status"].contains("message") && span_details["status"]["message"].is_string()) {
                task_bytes += span_details["status"]["message"].get<std::string>().size();
            }
            if (span_details.contains("attributes") && span_details["attributes"].is_array()) {
                for (const auto& attr : span_details["attributes"]) {
                    task_bytes += attr.value("key", "").size();
                    if (attr.contains("value") && attr["value"].contains("stringValue")) {
                        task_bytes += attr["value"]["stringValue"].get<std::string>().size();
                    } else {
                        task_bytes += 16;
                    }
                }
            }
        }

        std::unique_lock<std::mutex> lock(mutex_);
        if (shutdown_) return;

        if (endpoint != last_endpoint_ || !last_enabled_) {
            last_endpoint_ = endpoint;
            last_enabled_ = true;
            endpoint_unreachable_ = false;
        }

        size_t max_capacity = MAX_CAPACITY;
        int64_t max_queue_bytes = 134217728; // 128MB default
        if (auto* config = RuntimeConfig::global()) {
            max_capacity = static_cast<size_t>(config->telemetry_max_queue_capacity());
            max_queue_bytes = config->telemetry_max_queue_bytes();
        }

        size_t max_bytes = 0;
        if (max_queue_bytes > 0) {
            if (static_cast<uint64_t>(max_queue_bytes) > std::numeric_limits<size_t>::max()) {
                max_bytes = std::numeric_limits<size_t>::max();
            } else {
                max_bytes = static_cast<size_t>(max_queue_bytes);
            }
        }

        if (max_bytes > 0 && task_bytes > max_bytes) {
            dropped_spans_count_++;
            if (dropped_spans_count_ % 100 == 1) {
                LOG(WARNING, "Telemetry") << "Single span size (" << task_bytes
                                          << " bytes) exceeds max_queue_bytes (" << max_bytes
                                          << " bytes). Dropped span immediately. Total dropped: "
                                          << dropped_spans_count_ << std::endl;
            }
            return;
        }

        while (!queue_.empty() && (
            (max_bytes > 0 && current_bytes_ + task_bytes > max_bytes) ||
            queue_.size() >= max_capacity
        )) {
            dropped_spans_count_++;
            if (dropped_spans_count_ % 100 == 1) {
                LOG(WARNING, "Telemetry") << "Telemetry queue limit reached (spans: " << queue_.size()
                                          << "/" << max_capacity << ", bytes: " << current_bytes_
                                          << "/" << max_bytes << "). Dropped oldest span. Total dropped: "
                                          << dropped_spans_count_ << std::endl;
            }
            pop_front_task();
        }

        int batch_size = 100;
        if (auto* config = RuntimeConfig::global()) {
            batch_size = config->telemetry_otlp_send_batch_size();
        }
        LOG(DEBUG, "Telemetry") << "Accumulating span to batch (size " << (queue_.size() + 1) << "/" << batch_size << ")..." << std::endl;

        current_bytes_ += task_bytes;
        if (protocol == "http/json") {
            queue_.push_back({std::move(json_str), {}, std::move(endpoint), std::move(headers), std::move(protocol), std::chrono::steady_clock::now(), task_bytes});
        } else {
            queue_.push_back({{}, std::move(span_details), std::move(endpoint), std::move(headers), std::move(protocol), std::chrono::steady_clock::now(), task_bytes});
        }
        cv_.notify_one();
    }

    size_t current_bytes() const {
        std::unique_lock<std::mutex> lock(mutex_);
        return current_bytes_;
    }

    size_t size() const {
        std::unique_lock<std::mutex> lock(mutex_);
        return queue_.size();
    }

    size_t dropped_count() const {
        std::unique_lock<std::mutex> lock(mutex_);
        return dropped_spans_count_;
    }
};

} // namespace lemon::telemetry
