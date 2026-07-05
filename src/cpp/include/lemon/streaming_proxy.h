#pragma once

#include <string>
#include <functional>
#include <nlohmann/json.hpp>
#include <httplib.h>
#include "utils/http_client.h"
#include "utils/aixlog.hpp"
#include <iomanip>

namespace lemon {

using json = nlohmann::json;

class StreamingProxy {
public:
    struct TelemetryData {
        int input_tokens = 0;
        int output_tokens = 0;
        double time_to_first_token = 0.0;
        double tokens_per_second = 0.0;
        std::string error_message = "";
        std::string model_name = "";

        void print() const {
            if (input_tokens > 0 || output_tokens > 0) {
                LOG(INFO, "Telemetry") << "Inference completed: model=" << model_name
                                       << ", tokens=" << (input_tokens + output_tokens)
                                       << " (in=" << input_tokens << ", out=" << output_tokens << ")"
                                       << ", ttft=" << std::fixed << std::setprecision(3) << time_to_first_token << "s"
                                       << ", tps=" << std::fixed << std::setprecision(2) << tokens_per_second << std::endl;

                LOG(DEBUG, "Telemetry") << "=== Telemetry ===\n"
                                        << "Model:         " << model_name << "\n"
                                        << "Input tokens:  " << input_tokens << "\n"
                                        << "Output tokens: " << output_tokens << "\n"
                                        << "TTFT (s):      " << std::fixed << std::setprecision(3) << time_to_first_token << "\n"
                                        << "TPS:           " << std::fixed << std::setprecision(2) << tokens_per_second << "\n"
                                        << "=================" << std::endl;
            }
        }
    };

    static void forward_sse_stream(
        const std::string& backend_url,
        const std::string& request_body,
        httplib::DataSink& sink,
        std::function<void(const TelemetryData&)> on_complete = nullptr,
        long timeout_seconds = 300,
        std::function<void()> on_chunk = nullptr
    );

    static void forward_byte_stream(
        const std::string& backend_url,
        const std::string& request_body,
        httplib::DataSink& sink,
        long timeout_seconds = 300,
        std::function<void()> on_chunk = nullptr
    );

    static void process_sse_lines(std::string& line_buffer, std::function<void(const std::string&)> line_callback);

    static TelemetryData parse_telemetry(const std::string& buffer);

    static void accumulate_responses_delta(const nlohmann::json& parsed, std::string& accumulated_text);

private:
};

} // namespace lemon
