#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "utils/aixlog.hpp"

namespace lemon {

using json = nlohmann::json;

struct LogStreamEntry {
    uint64_t seq = 0;
    std::string timestamp;
    std::string severity;
    std::string tag;
    std::string line;

    json to_json() const;
};

class LogStreamHub {
public:
    using SubscriberCallback = std::function<void(const LogStreamEntry&)>;

    static LogStreamHub& instance();

    std::vector<LogStreamEntry> snapshot(std::optional<uint64_t> after_seq = std::nullopt) const;
    std::string add_subscriber(SubscriberCallback callback);
    void remove_subscriber(const std::string& subscriber_id);

    std::vector<std::shared_ptr<AixLog::Sink>> create_sinks(
        const std::string& log_level,
        bool include_console) const;

    void publish(const AixLog::Metadata& metadata, const std::string& formatted_line);

private:
    LogStreamHub() = default;

    std::string next_subscriber_id();
    static std::string resolve_tag(const AixLog::Metadata& metadata);
    static std::string resolve_timestamp(const AixLog::Metadata& metadata);

    static constexpr size_t kMaxRetainedEntries = 5000;

    mutable std::mutex mutex_;
    std::deque<LogStreamEntry> entries_;
    std::unordered_map<std::string, SubscriberCallback> subscribers_;
    std::atomic<uint64_t> next_seq_{1};
    std::atomic<uint64_t> next_subscriber_{1};
};

void configure_application_logging(const std::string& log_level, bool include_console);

} // namespace lemon
