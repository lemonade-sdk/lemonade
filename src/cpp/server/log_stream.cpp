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

std::string LogStreamHub::subscribe_with_snapshot(
    SubscriberCallback callback,
    std::optional<uint64_t> after_seq,
    std::vector<LogStreamEntry>& out_snapshot) {
    std::lock_guard<std::mutex> lock(mutex_);

    out_snapshot.clear();
    out_snapshot.reserve(entries_.size());
    for (const auto& entry : entries_) {
        if (!after_seq.has_value() || entry.seq > *after_seq) {
            out_snapshot.push_back(entry);
        }
    }

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
    entry.timestamp = resolve_timestamp(metadata);
    entry.severity = AixLog::to_string(metadata.severity);
    entry.tag = resolve_tag(metadata);
    entry.line = formatted_line;

    std::vector<SubscriberCallback> callbacks;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        entry.seq = next_seq_++;
        entries_.push_back(entry);
        while (entries_.size() > kMaxRetainedEntries) {
            entries_.pop_front();
        }

        callbacks.reserve(subscribers_.size());
        for (const auto& [_, callback] : subscribers_) {
            callbacks.push_back(callback);
        }
    }

    // Invoke callbacks outside the lock to avoid deadlocking if a callback
    // ends up logging (which would re-enter publish via the HubPublishingSink).
    for (const auto& callback : callbacks) {
        callback(entry);
    }
}

std::string LogStreamHub::next_subscriber_id() {
    return "log-sub-" + std::to_string(next_subscriber_++);
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
