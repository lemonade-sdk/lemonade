#pragma once

#include <string>
#include <functional>
#include <nlohmann/json.hpp>
#include <httplib.h>
#include "utils/http_client.h"

namespace lemon {

using json = nlohmann::json;

class StreamingProxy {
public:
    struct TelemetryData {
        int input_tokens = 0;
        int output_tokens = 0;
        double time_to_first_token = 0.0;
        double tokens_per_second = 0.0;
        
        void print() const {
            if (input_tokens > 0 || output_tokens > 0) {
                std::cout << "\n=== Telemetry ===" << std::endl;
                std::cout << "Input tokens:  " << input_tokens << std::endl;
                std::cout << "Output tokens: " << output_tokens << std::endl;
                std::cout << "TTFT (s):      " << std::fixed << std::setprecision(3) 
                          << time_to_first_token << std::endl;
                std::cout << "TPS:           " << std::fixed << std::setprecision(2) 
                          << tokens_per_second << std::endl;
                std::cout << "=================" << std::endl;
            }
        }
    };
    
    // Stream a request to backend and forward SSE chunks to client
    static void forward_sse_stream(
        const std::string& backend_url,
        const std::string& request_body,
        httplib::DataSink& sink,
        std::function<void(const TelemetryData&)> on_complete = nullptr
    );
    
private:
    // Parse telemetry from SSE chunks
    static TelemetryData parse_telemetry(const std::string& buffer);
};

} // namespace lemon
