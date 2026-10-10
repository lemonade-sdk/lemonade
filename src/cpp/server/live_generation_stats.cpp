#include "lemon/live_generation_stats.h"

#include <algorithm>
#include <utility>

namespace lemon {

namespace {

constexpr auto kRateWindow = std::chrono::seconds(5);
constexpr double kRateWindowSeconds = 5.0;

} // namespace

nlohmann::json LiveGenerationStats::Snapshot::to_json() const {
    nlohmann::json stats = {
        {"inference_active", active_requests > 0},
        {"live_active_requests", active_requests},
        {"live_generated_chunks", generated_chunks},
        {"live_generation_rate_estimate", generation_rate_estimate},
        {"live_generation_rate_unit", "semantic_sse_chunks_per_second"},
        {"live_generation_recipe", "flm"},
        {"live_generation_device", "npu"}
    };
    if (model_name.has_value()) {
        stats["live_generation_model"] = *model_name;
    }
    return stats;
}

LiveGenerationStats::Request::~Request() {
    reset();
}

LiveGenerationStats::Request::Request(Request&& other) noexcept
    : stats_(std::exchange(other.stats_, nullptr)),
      request_id_(std::exchange(other.request_id_, 0)) {}

LiveGenerationStats::Request& LiveGenerationStats::Request::operator=(Request&& other) noexcept {
    if (this != &other) {
        reset();
        stats_ = std::exchange(other.stats_, nullptr);
        request_id_ = std::exchange(other.request_id_, 0);
    }
    return *this;
}

void LiveGenerationStats::Request::record_semantic_chunk(Clock::time_point now) {
    if (stats_) {
        stats_->record_semantic_chunk(request_id_, now);
    }
}

void LiveGenerationStats::Request::reset() {
    if (stats_) {
        stats_->finish(request_id_);
        stats_ = nullptr;
        request_id_ = 0;
    }
}

LiveGenerationStats::RequestId LiveGenerationStats::begin(const std::string& model_name,
                                                           Clock::time_point) {
    std::lock_guard<std::mutex> lock(mutex_);
    const RequestId request_id = next_request_id_++;
    active_requests_.emplace(request_id, ActiveRequest{model_name});
    return request_id;
}

void LiveGenerationStats::finish(RequestId request_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    active_requests_.erase(request_id);
}

void LiveGenerationStats::record_semantic_chunk(RequestId request_id, Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = active_requests_.find(request_id);
    if (it == active_requests_.end()) {
        return;
    }

    ActiveRequest& request = it->second;
    if (!request.has_first_event) {
        request.first_event = now;
        request.has_first_event = true;
    }
    request.generated_chunks++;
    request.recent_events.push_back(now);
    prune_expired_events(request, now);
}

LiveGenerationStats::Request LiveGenerationStats::track(const std::string& model_name,
                                                         Clock::time_point now) {
    return Request(this, begin(model_name, now));
}

LiveGenerationStats::Snapshot LiveGenerationStats::snapshot(Clock::time_point now) const {
    std::lock_guard<std::mutex> lock(mutex_);

    Snapshot snapshot;
    std::optional<std::string> shared_model_name;
    bool model_names_match = true;

    for (auto& entry : active_requests_) {
        ActiveRequest& request = entry.second;
        snapshot.active_requests++;
        snapshot.generated_chunks += request.generated_chunks;

        if (!shared_model_name.has_value()) {
            shared_model_name = request.model_name;
        } else if (*shared_model_name != request.model_name) {
            model_names_match = false;
        }

        prune_expired_events(request, now);
        if (!request.has_first_event || request.recent_events.empty()) {
            continue;
        }

        const double elapsed_seconds = std::chrono::duration<double>(now - request.first_event).count();
        const double denominator = std::max(1.0, std::min(kRateWindowSeconds, elapsed_seconds));
        snapshot.generation_rate_estimate +=
            static_cast<double>(request.recent_events.size()) / denominator;
    }

    if (model_names_match && shared_model_name.has_value() && !shared_model_name->empty()) {
        snapshot.model_name = std::move(shared_model_name);
    }
    return snapshot;
}

void LiveGenerationStats::prune_expired_events(ActiveRequest& request, Clock::time_point now) {
    const Clock::time_point cutoff = now - kRateWindow;
    while (!request.recent_events.empty() && request.recent_events.front() < cutoff) {
        request.recent_events.pop_front();
    }
}

} // namespace lemon
