#include "lemon/logging_config.h"

#include "lemon/log_stream.h"
#include "lemon/runtime_config.h"
#include "lemon/system_info.h"
#include "lemon/utils/path_utils.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <utility>
#include <vector>

namespace lemon {

namespace fs = std::filesystem;

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

} // namespace

RotatingFileSink::RotatingFileSink(const AixLog::Filter& filter,
                                   const std::string& filename,
                                   const std::string& format,
                                   size_t max_file_size_mb,
                                   size_t max_files)
    : AixLog::SinkFormat(filter, format),
      filename_(filename),
      max_file_size_bytes_(max_file_size_mb * 1024 * 1024),
      max_files_(max_files) {
    if (max_file_size_mb < 1 || max_file_size_mb > 2048) {
        throw std::invalid_argument("'log_max_file_size_mb' must be between 1 and 2048");
    }
    if (max_files > 100) {
        throw std::invalid_argument("'log_max_files' must be between 0 and 100");
    }

    std::error_code ec;
    fs::path p = utils::path_from_utf8(filename_);
    if (fs::exists(p, ec) && fs::is_directory(p, ec)) {
        throw std::invalid_argument("Log file path cannot be a directory: " + filename_);
    }
    if (p.has_parent_path()) {
        fs::create_directories(p.parent_path(), ec);
    }
    if (fs::exists(p, ec)) {
        if (fs::is_directory(p, ec)) {
            throw std::invalid_argument("Log file path cannot be a directory: " + filename_);
        }
        current_size_ = static_cast<size_t>(fs::file_size(p, ec));
    }

    if (max_file_size_bytes_ > 0 && current_size_ >= max_file_size_bytes_) {
        rotate_if_needed_nolock();
    } else {
        file_.open(p, std::ofstream::out | std::ofstream::app | std::ofstream::binary);
    }
}

RotatingFileSink::~RotatingFileSink() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_.is_open()) {
        file_.flush();
        file_.close();
    }
}

void RotatingFileSink::log(const AixLog::Metadata& metadata, const std::string& message) {
    std::ostringstream stream;
    do_log(stream, metadata, message);

    std::string formatted = stream.str();
    if (!formatted.empty() && formatted.back() == '\n') {
        formatted.pop_back();
    }

    size_t line_len = formatted.size() + 1; // +1 for newline

    std::lock_guard<std::mutex> lock(mutex_);
    if (!file_.is_open()) {
        return;
    }

    if (max_file_size_bytes_ > 0 && current_size_ > 0 && (current_size_ + line_len) >= max_file_size_bytes_) {
        rotate_if_needed_nolock();
    }

    file_ << formatted << '\n';
    file_.flush();
    current_size_ += line_len;
}

size_t RotatingFileSink::current_size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_size_;
}

void RotatingFileSink::rotate_if_needed_nolock() {
    if (file_.is_open()) {
        file_.flush();
        file_.close();
    }

    std::error_code ec;
    fs::path active_log = utils::path_from_utf8(filename_);

    if (max_files_ > 0) {
        fs::path max_backup = utils::path_from_utf8(filename_ + "." + std::to_string(max_files_));
        if (fs::exists(max_backup, ec)) {
            fs::remove(max_backup, ec);
        }

        for (size_t i = max_files_; i > 1; --i) {
            fs::path dst = utils::path_from_utf8(filename_ + "." + std::to_string(i));
            fs::path src = utils::path_from_utf8(filename_ + "." + std::to_string(i - 1));
            if (fs::exists(src, ec)) {
                if (fs::exists(dst, ec)) {
                    fs::remove(dst, ec);
                }
                fs::rename(src, dst, ec);
            }
        }

        fs::path backup_1 = utils::path_from_utf8(filename_ + ".1");
        if (fs::exists(active_log, ec)) {
            if (fs::exists(backup_1, ec)) {
                fs::remove(backup_1, ec);
            }
            fs::rename(active_log, backup_1, ec);
        }
    }

    file_.open(active_log, std::ofstream::out | std::ofstream::trunc | std::ofstream::binary);
    if (!file_.is_open()) {
        file_.open(active_log, std::ofstream::out | std::ofstream::app | std::ofstream::binary);
    }

    if (fs::exists(active_log, ec)) {
        current_size_ = static_cast<size_t>(fs::file_size(active_log, ec));
    } else {
        current_size_ = 0;
    }
}

namespace {

std::vector<std::shared_ptr<AixLog::Sink>> build_logging_sinks(
    const std::string& log_level,
    const LoggingTargets& targets) {
    auto filter = AixLog::Filter(AixLog::to_severity(log_level));

    std::vector<std::shared_ptr<AixLog::Sink>> sinks;
    if (targets.console) {
        sinks.push_back(std::make_shared<AixLog::SinkCout>(filter, RuntimeConfig::LOG_FORMAT));
    }
    if (targets.file && targets.file_path.has_value()) {
        sinks.push_back(std::make_shared<RotatingFileSink>(
            filter,
            *targets.file_path,
            RuntimeConfig::LOG_FORMAT,
            targets.rotation.max_file_size_mb,
            targets.rotation.max_files));
    }
    if (targets.stream_hub) {
        sinks.push_back(std::make_shared<HubPublishingSink>(filter, RuntimeConfig::LOG_FORMAT));
    }

    return sinks;
}

LoggingMode& active_logging_mode() {
    static LoggingMode mode = LoggingMode::direct_server;
    return mode;
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

LoggingTargets resolve_logging_targets(LoggingMode mode, const LogRotationConfig& rotation) {
    LoggingTargets targets;
    targets.stream_hub = true;
    targets.rotation = rotation;

    switch (mode) {
    case LoggingMode::direct_server:
        targets.console = true;
        if (rotation.file_mode == "enabled") {
            targets.file = true;
        } else if (rotation.file_mode == "disabled" || rotation.file_mode == "auto" || rotation.file_mode.empty()) {
            targets.file = false;
        } else {
            targets.file = true;
        }
        break;
    case LoggingMode::embedded_tray_server:
        targets.console = false;
        if (rotation.file_mode == "disabled") {
            targets.file = false;
        } else {
            targets.file = true;
        }
        break;
    }

    if (targets.file) {
        if (rotation.file_mode != "enabled" && rotation.file_mode != "disabled" && rotation.file_mode != "auto" && !rotation.file_mode.empty()) {
            targets.file_path = rotation.file_mode;
        } else {
            fs::path p = utils::path_from_utf8(utils::get_runtime_dir()) / "lemonade-server.log";
            targets.file_path = utils::path_to_utf8(p);
        }
    }

    return targets;
}

void configure_application_logging(const std::string& log_level, LoggingMode mode, const LogRotationConfig& rotation) {
    std::lock_guard<std::mutex> lock(logging_config_mutex());
    active_logging_mode() = mode;
    const LoggingTargets targets = resolve_logging_targets(mode, rotation);
    active_logging_targets() = targets;
    active_logging_targets_initialized() = true;
    AixLog::Log::init(build_logging_sinks(log_level, targets));
}

void reconfigure_application_logging(const std::string& log_level, const LogRotationConfig& rotation) {
    std::lock_guard<std::mutex> lock(logging_config_mutex());

    const LoggingTargets targets = resolve_logging_targets(active_logging_mode(), rotation);
    active_logging_targets() = targets;
    active_logging_targets_initialized() = true;

    AixLog::Log::init(build_logging_sinks(log_level, active_logging_targets()));
}

} // namespace lemon
