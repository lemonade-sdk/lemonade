#pragma once

#include <string>
#include <map>
#include <functional>
#include <chrono>
#include <memory>
#include <iostream>
#include <iomanip>

namespace lemon {
namespace utils {

struct HttpResponse {
    int status_code;
    std::string body;
    std::map<std::string, std::string> headers;
};

using ProgressCallback = std::function<void(size_t downloaded, size_t total)>;
using StreamCallback = std::function<bool(const char* data, size_t length)>;

class HttpClient {
public:
    // Simple GET request
    static HttpResponse get(const std::string& url,
                           const std::map<std::string, std::string>& headers = {});
    
    // Simple POST request
    static HttpResponse post(const std::string& url,
                            const std::string& body,
                            const std::map<std::string, std::string>& headers = {});
    
    // Streaming POST request (calls callback for each chunk as it arrives)
    static HttpResponse post_stream(const std::string& url,
                                   const std::string& body,
                                   StreamCallback stream_callback,
                                   const std::map<std::string, std::string>& headers = {});
    
    // Download file to disk
    static bool download_file(const std::string& url,
                             const std::string& output_path,
                             ProgressCallback callback = nullptr,
                             const std::map<std::string, std::string>& headers = {});
    
    // Check if URL is reachable
    static bool is_reachable(const std::string& url, int timeout_seconds = 5);
};

// Helper function to create a throttled progress callback for file downloads
// Returns a callback that prints progress at most once per second
inline ProgressCallback create_throttled_progress_callback() {
    auto last_print_time = std::make_shared<std::chrono::steady_clock::time_point>(
        std::chrono::steady_clock::now());
    auto printed_final = std::make_shared<bool>(false);
    
    return [last_print_time, printed_final](size_t current, size_t total) {
        if (total > 0) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - *last_print_time);
            
            bool is_complete = (current >= total);
            
            // Skip if we've already printed the final progress
            if (is_complete && *printed_final) {
                return;
            }
            
            // Print if it's been more than 1 second, or if download just completed
            if (elapsed.count() >= 1000 || (is_complete && !*printed_final)) {
                // Always show 100% when complete (avoid 99% due to rounding)
                int percent = is_complete ? 100 : static_cast<int>((current * 100) / total);
                double mb_current = current / (1024.0 * 1024.0);
                double mb_total = total / (1024.0 * 1024.0);
                std::cout << "\r  Progress: " << percent << "% (" 
                         << std::fixed << std::setprecision(1) 
                         << mb_current << "/" << mb_total << " MB)" << std::flush;
                *last_print_time = now;
                
                if (is_complete) {
                    *printed_final = true;
                }
            }
        }
    };
}

} // namespace utils
} // namespace lemon

