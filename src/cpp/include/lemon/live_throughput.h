#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string_view>
#include <unordered_map>

namespace lemon {

// Best-effort output throughput. Inference events must be filtered before
// observe(); only final backend usage contains authoritative token counts.
class LiveThroughput {
public:
    using Clock = std::chrono::steady_clock;

    struct Snapshot {
        size_t active_requests = 0;
        double estimated_tokens_per_second = 0.0;
    };

    class Request {
    public:
        Request(const Request&) = delete;
        Request& operator=(const Request&) = delete;
        ~Request() { owner_.finish(id_); }

        void observe(std::string_view text, Clock::time_point now = Clock::now()) {
            // Approximately four ASCII chars or one Unicode code point/token.
            double tokens = 0.0;
            for (unsigned char c : text) {
                if (c < 0x80) tokens += 0.25;
                else if ((c & 0xc0) != 0x80) tokens += 1.0;
            }
            if (tokens > 0.0) owner_.observe(id_, tokens, now);
        }

    private:
        friend class LiveThroughput;
        Request(LiveThroughput& owner, uint64_t id) : owner_(owner), id_(id) {}
        LiveThroughput& owner_;
        uint64_t id_;
    };

    Request start() {
        std::lock_guard<std::mutex> lock(mutex_);
        // Starting or pre-filling a request is not token generation.
        return Request(*this, ++next_id_);
    }

    Snapshot snapshot(Clock::time_point now = Clock::now()) const {
        std::lock_guard<std::mutex> lock(mutex_);
        Snapshot result;
        for (const auto& [id, stream] : requests_) {
            (void)id;
            // A quiet or completed stream must not look like ongoing output.
            if (stream.samples.empty() || now - stream.last > std::chrono::seconds(3)) continue;
            ++result.active_requests;
            if (stream.samples.size() < 2) continue;
            const auto& first = stream.samples.front();
            const auto seconds = std::chrono::duration<double>(now - first.time).count();
            if (seconds >= 0.2)
                result.estimated_tokens_per_second += (stream.total - first.tokens) / seconds;
        }
        return result;
    }

private:
    struct Sample {
        Clock::time_point time;
        double tokens;
    };
    struct Stream {
        double total = 0.0;
        Clock::time_point last{};
        std::deque<Sample> samples;
    };

    void observe(uint64_t id, double tokens, Clock::time_point now) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& stream = requests_[id]; // Register only after actual output.
        if (stream.samples.empty()) stream.samples.push_back({now, 0.0});
        stream.total += tokens;
        stream.last = now;
        stream.samples.push_back({now, stream.total});
        const auto cutoff = now - std::chrono::seconds(5);
        while (stream.samples.size() > 1 && stream.samples.front().time < cutoff)
            stream.samples.pop_front();
    }

    void finish(uint64_t id) {
        std::lock_guard<std::mutex> lock(mutex_);
        requests_.erase(id);
    }

    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, Stream> requests_;
    uint64_t next_id_ = 0;
};

} // namespace lemon
