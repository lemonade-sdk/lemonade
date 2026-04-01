#include "lemon/log_stream.h"

#include <utility>

namespace lemon {

json LogStreamEntry::to_json() const {
    return {
        {"seq", seq},
        {"timestamp", timestamp},
        {"severity", severity},
        {"tag", tag},
        {"line", line},
    };
}

LogStreamHub& LogStreamHub::instance() {
    static LogStreamHub hub;
    return hub;
}

std::vector<LogStreamEntry> LogStreamHub::snapshot(std::optional<uint64_t> after_seq) const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<LogStreamEntry> snapshot_entries;
    snapshot_entries.reserve(entries_.size());

    for (const auto& entry : entries_) {
        if (!after_seq.has_value() || entry.seq > *after_seq) {
            snapshot_entries.push_back(entry);
        }
    }

    return snapshot_entries;
}

std::string LogStreamHub::add_subscriber(SubscriberCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string subscriber_id = next_subscriber_id();
    subscribers_.emplace(subscriber_id, std::move(callback));
    return subscriber_id;
}

void LogStreamHub::remove_subscriber(const std::string& subscriber_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    subscribers_.erase(subscriber_id);
}

void LogStreamHub::publish(const AixLog::Metadata& metadata, const std::string& formatted_line) {
    LogStreamEntry entry;
    entry.seq = next_seq_.fetch_add(1);
    entry.timestamp = resolve_timestamp(metadata);
    entry.severity = AixLog::to_string(metadata.severity);
    entry.tag = resolve_tag(metadata);
    entry.line = formatted_line;

    std::vector<SubscriberCallback> callbacks;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.push_back(entry);
        while (entries_.size() > kMaxRetainedEntries) {
            entries_.pop_front();
        }

        callbacks.reserve(subscribers_.size());
        for (const auto& [_, callback] : subscribers_) {
            callbacks.push_back(callback);
        }
    }

    for (const auto& callback : callbacks) {
        callback(entry);
    }
}

std::string LogStreamHub::next_subscriber_id() {
    return "log-sub-" + std::to_string(next_subscriber_.fetch_add(1));
}

std::string LogStreamHub::resolve_tag(const AixLog::Metadata& metadata) {
    if (metadata.tag) {
        return metadata.tag.text;
    }
    if (metadata.function) {
        return metadata.function.name;
    }
    return "log";
}

std::string LogStreamHub::resolve_timestamp(const AixLog::Metadata& metadata) {
    if (metadata.timestamp) {
        return metadata.timestamp.to_string("%Y-%m-%d %H:%M:%S.#ms");
    }
    return "";
}

} // namespace lemon
