#pragma once

#include <optional>
#include <string>

namespace lemon {

enum class LoggingMode {
    direct_server,
    embedded_tray_server,
};

struct LoggingTargets {
    bool console = false;
    bool stream_hub = true;
    bool file = false;
    std::optional<std::string> file_path;
};

LoggingTargets resolve_logging_targets(LoggingMode mode);
void configure_application_logging(const std::string& log_level, LoggingMode mode);
void reconfigure_application_logging(const std::string& log_level);

} // namespace lemon
