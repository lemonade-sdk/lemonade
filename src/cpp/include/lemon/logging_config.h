#pragma once

#include "lemon/utils/aixlog.hpp"

#include <fstream>
#include <mutex>
#include <optional>
#include <string>

namespace lemon {

enum class LoggingMode {
    direct_server,
    embedded_tray_server,
};

struct LogRotationConfig {
    std::string file_mode = "auto";
    size_t max_file_size_mb = 10;
    size_t max_files = 5;
};

struct LoggingTargets {
    bool console = false;
    bool stream_hub = true;
    bool file = false;
    std::optional<std::string> file_path;
    LogRotationConfig rotation;
};

class RotatingFileSink : public AixLog::SinkFormat {
public:
    RotatingFileSink(const AixLog::Filter& filter,
                     const std::string& filename,
                     const std::string& format,
                     size_t max_file_size_mb,
                     size_t max_files);
    ~RotatingFileSink() override;

    void log(const AixLog::Metadata& metadata, const std::string& message) override;

    size_t current_size() const;

private:
    void rotate_if_needed_nolock();
    void prune_excess_backups_nolock();

    std::string filename_;
    size_t max_file_size_bytes_;
    size_t max_files_;
    size_t current_size_{0};
    std::ofstream file_;
    mutable std::mutex mutex_;
};

LoggingTargets resolve_logging_targets(LoggingMode mode, const LogRotationConfig& rotation = {});
void configure_application_logging(const std::string& log_level, LoggingMode mode, const LogRotationConfig& rotation = {});
void reconfigure_application_logging(const std::string& log_level, const LogRotationConfig& rotation = {});

} // namespace lemon
