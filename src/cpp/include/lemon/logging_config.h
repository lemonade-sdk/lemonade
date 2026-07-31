#pragma once

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

LoggingTargets resolve_logging_targets(LoggingMode mode, const LogRotationConfig& rotation = {});
void configure_application_logging(const std::string& log_level, LoggingMode mode, const LogRotationConfig& rotation = {});
void reconfigure_application_logging(const std::string& log_level, const LogRotationConfig& rotation = {});

} // namespace lemon
