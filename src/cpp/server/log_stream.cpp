#include "lemon/log_stream.h"

#include "lemon/runtime_config.h"
#include "lemon/utils/path_utils.h"

#include <fstream>
#include <sstream>
#include <utility>

namespace lemon {

namespace {

class LogStreamSink : public AixLog::SinkFormat {
public:
    LogStreamSink(const AixLog::Filter& filter,
                  const std::string& filename,
                  const std::string& format)
        : AixLog::SinkFormat(filter, format),
          file_(filename.c_str(), std::ofstream::out | std::ofstream::trunc) {
    }

    void log(const AixLog::Metadata& metadata, const std::string& message) override {
        std::ostringstream stream;
        do_log(stream, metadata, message);

        std::string formatted = stream.str();
        if (!formatted.empty() && formatted.back() == '\n') {
            formatted.pop_back();
        }

        file_ << formatted << std::endl;
        file_.flush();

        LogStreamHub::instance().publish(metadata, formatted);
    }

private:
    std::ofstream file_;
};

std::string log_file_path() {
#ifdef _WIN32
    return utils::get_runtime_dir() + "lemonade-server.log";
#else
    return utils::get_runtime_dir() + "/lemonade-server.log";
#endif
}

} // namespace

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

std::vector<std::shared_ptr<AixLog::Sink>> LogStreamHub::create_sinks(
    const std::string& log_level,
    bool include_console) const {
    auto filter = AixLog::Filter(AixLog::to_severity(log_level));

    std::vector<std::shared_ptr<AixLog::Sink>> sinks;
    if (include_console) {
        sinks.push_back(std::make_shared<AixLog::SinkCout>(filter, RuntimeConfig::LOG_FORMAT));
    }
    sinks.push_back(std::make_shared<LogStreamSink>(filter, log_file_path(), RuntimeConfig::LOG_FORMAT));

    return sinks;
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

void configure_application_logging(const std::string& log_level, bool include_console) {
    AixLog::Log::init(LogStreamHub::instance().create_sinks(log_level, include_console));
}

} // namespace lemon
