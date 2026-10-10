#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace lemon {

class LiveGenerationStats {
public:
    using Clock = std::chrono::steady_clock;
    using RequestId = uint64_t;

    struct Snapshot {
        uint64_t active_requests = 0;
        uint64_t generated_chunks = 0;
        double generation_rate_estimate = 0.0;
        std::optional<std::string> model_name;

        nlohmann::json to_json() const;
    };

    class Request {
    public:
        Request() = default;
        ~Request();

        Request(const Request&) = delete;
        Request& operator=(const Request&) = delete;
        Request(Request&& other) noexcept;
        Request& operator=(Request&& other) noexcept;

        void record_semantic_chunk(Clock::time_point now = Clock::now());
        void reset();
        explicit operator bool() const { return stats_ != nullptr; }

    private:
        friend class LiveGenerationStats;
        Request(LiveGenerationStats* stats, RequestId request_id)
            : stats_(stats), request_id_(request_id) {}

        LiveGenerationStats* stats_ = nullptr;
        RequestId request_id_ = 0;
    };

    RequestId begin(const std::string& model_name, Clock::time_point now = Clock::now());
    void finish(RequestId request_id);
    void record_semantic_chunk(RequestId request_id, Clock::time_point now = Clock::now());
    Request track(const std::string& model_name, Clock::time_point now = Clock::now());
    Snapshot snapshot(Clock::time_point now = Clock::now()) const;

private:
    struct ActiveRequest {
        std::string model_name;
        uint64_t generated_chunks = 0;
        bool has_first_event = false;
        Clock::time_point first_event;
        std::deque<Clock::time_point> recent_events;
    };

    static void prune_expired_events(ActiveRequest& request, Clock::time_point now);

    mutable std::mutex mutex_;
    mutable std::unordered_map<RequestId, ActiveRequest> active_requests_;
    RequestId next_request_id_ = 1;
};

} // namespace lemon
