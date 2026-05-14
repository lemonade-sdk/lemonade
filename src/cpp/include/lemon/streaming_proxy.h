#pragma once

#include <functional>
#include <iomanip>
#include <sstream>
#include <string>
#include <nlohmann/json.hpp>
#include <httplib.h>
#include "utils/http_client.h"
#include "utils/aixlog.hpp"

namespace lemon {

using json = nlohmann::json;

class StreamingProxy {
public:
    struct TelemetryData {
        int input_tokens = 0;
        int output_tokens = 0;
        double time_to_first_token = 0.0;
        double tokens_per_second = 0.0;

        bool has_tokens() const {
            return input_tokens > 0 || output_tokens > 0;
        }

        std::string to_log_banner() const {
            std::ostringstream ss;
            ss << "=== Telemetry ===\n";
            ss << "Input tokens:  " << input_tokens << "\n";
            ss << "Output tokens: " << output_tokens << "\n";
            ss << "TTFT (s):      " << std::fixed << std::setprecision(3)
               << time_to_first_token << "\n";
            ss << "TPS:           " << std::fixed << std::setprecision(2)
               << tokens_per_second << "\n";
            ss << "=================";
            return ss.str();
        }

        void print() const {
            if (input_tokens > 0 || output_tokens > 0) {
                LOG(INFO, "Server") << "Streaming completed - 200 OK" << std::endl;
                LOG(INFO, "Telemetry") << to_log_banner() << std::endl;
            } else {
                LOG(INFO, "Server") << "Streaming completed - 200 OK" << std::endl;
            }
        }
    };

    // Stream a request to backend and forward SSE chunks to client
    static void forward_sse_stream(
        const std::string& backend_url,
        const std::string& request_body,
        httplib::DataSink& sink,
        std::function<void(const TelemetryData&)> on_complete = nullptr,
        long timeout_seconds = 300
    );

    static void forward_byte_stream(
        const std::string& backend_url,
        const std::string& request_body,
        httplib::DataSink& sink,
        std::function<void(const TelemetryData&)> on_complete = nullptr,
        long timeout_seconds = 300
    );

private:
    // Parse telemetry from SSE chunks
    static TelemetryData parse_telemetry(const std::string& buffer);
};

} // namespace lemon
