#include "lemon/logging_config.h"

#include "lemon/log_stream.h"
#include "lemon/runtime_config.h"
#include "lemon/system_info.h"
#include "lemon/utils/path_utils.h"

#include <fstream>
#include <mutex>
#include <sstream>
#include <utility>
#include <vector>

namespace lemon {

namespace {

class HubPublishingSink : public AixLog::SinkFormat {
public:
    HubPublishingSink(const AixLog::Filter& filter, const std::string& format)
        : AixLog::SinkFormat(filter, format) {
    }

    void log(const AixLog::Metadata& metadata, const std::string& message) override {
        std::ostringstream stream;
        do_log(stream, metadata, message);

        std::string formatted = stream.str();
        if (!formatted.empty() && formatted.back() == '\n') {
            formatted.pop_back();
        }

        LogStreamHub::instance().publish(metadata, formatted);
    }
};

class FileLogSink : public AixLog::SinkFormat {
public:
    FileLogSink(const AixLog::Filter& filter,
                const std::string& filename,
                const std::string& format)
        : AixLog::SinkFormat(filter, format),
          file_(filename.c_str(), std::ofstream::out | std::ofstream::app) {
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
    }

private:
    std::ofstream file_;
};

std::vector<std::shared_ptr<AixLog::Sink>> build_logging_sinks(
    const std::string& log_level,
    const LoggingTargets& targets) {
    auto filter = AixLog::Filter(AixLog::to_severity(log_level));

    std::vector<std::shared_ptr<AixLog::Sink>> sinks;
    if (targets.console) {
        sinks.push_back(std::make_shared<AixLog::SinkCout>(filter, RuntimeConfig::LOG_FORMAT));
    }
    if (targets.file && targets.file_path.has_value()) {
        sinks.push_back(std::make_shared<FileLogSink>(
            filter,
            *targets.file_path,
            RuntimeConfig::LOG_FORMAT));
    }
    if (targets.stream_hub) {
        sinks.push_back(std::make_shared<HubPublishingSink>(filter, RuntimeConfig::LOG_FORMAT));
    }

    return sinks;
}

LoggingTargets& active_logging_targets() {
    static LoggingTargets targets;
    return targets;
}

bool& active_logging_targets_initialized() {
    static bool initialized = false;
    return initialized;
}

std::mutex& logging_config_mutex() {
    static std::mutex mutex;
    return mutex;
}

} // namespace

LoggingTargets resolve_logging_targets(LoggingMode mode) {
    LoggingTargets targets;
    targets.stream_hub = true;

    switch (mode) {
    case LoggingMode::direct_server:
        targets.console = true;
        targets.file = !SystemInfo::is_running_under_systemd();
        break;
    case LoggingMode::embedded_tray_server:
        targets.console = false;
        targets.file = true;
        break;
    }

    if (targets.file) {
#ifdef _WIN32
        targets.file_path = utils::get_runtime_dir() + "lemonade-server.log";
#else
        targets.file_path = utils::get_runtime_dir() + "/lemonade-server.log";
#endif
    }

    return targets;
}

void configure_application_logging(const std::string& log_level, LoggingMode mode) {
    const LoggingTargets targets = resolve_logging_targets(mode);

    std::lock_guard<std::mutex> lock(logging_config_mutex());
    active_logging_targets() = targets;
    active_logging_targets_initialized() = true;
    AixLog::Log::init(build_logging_sinks(log_level, targets));
}

void reconfigure_application_logging(const std::string& log_level) {
    std::lock_guard<std::mutex> lock(logging_config_mutex());

    if (!active_logging_targets_initialized()) {
        active_logging_targets() = resolve_logging_targets(LoggingMode::direct_server);
        active_logging_targets_initialized() = true;
    }

    AixLog::Log::init(build_logging_sinks(log_level, active_logging_targets()));
}

} // namespace lemon
