#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include "utils/aixlog.hpp"
#include <nlohmann/json.hpp>

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

    std::string subscribe_with_snapshot(SubscriberCallback callback,
                                        std::optional<uint64_t> after_seq,
                                        std::vector<LogStreamEntry>& out_snapshot);
    void remove_subscriber(const std::string& subscriber_id);

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
    uint64_t next_seq_{1};
    uint64_t next_subscriber_{1};
};

} // namespace lemon
