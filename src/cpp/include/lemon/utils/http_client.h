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

// Result of a download operation with detailed error information
struct DownloadResult {
    bool success = false;
    bool cancelled = false;           // True if download was cancelled by user
    std::string error_message;
    std::string curl_error;           // CURL error string if applicable
    int curl_code = 0;                // CURL error code
    long http_code = 0;               // HTTP response code
    size_t bytes_downloaded = 0;      // Bytes downloaded in this attempt
    size_t total_bytes = 0;           // Total file size (if known)
    bool can_resume = false;          // Whether partial download can be resumed
};

// Progress callback returns bool: true = continue, false = cancel download
using ProgressCallback = std::function<bool(size_t downloaded, size_t total)>;
using StreamCallback = std::function<bool(const char* data, size_t length)>;

// Download configuration options
struct DownloadOptions {
    int max_retries = 5;              // Maximum retry attempts
    int initial_retry_delay_ms = 1000; // Initial delay between retries (doubles each time)
    int max_retry_delay_ms = 60000;   // Maximum delay between retries (1 minute)
    bool resume_partial = true;       // Resume partial downloads if possible
    int low_speed_limit = 1000;       // Minimum bytes/sec before timeout (1KB/s)
    int low_speed_time = 60;          // Seconds below low_speed_limit before timeout
    int connect_timeout = 30;         // Connection timeout in seconds
};

class HttpClient {
public:
    // Simple GET request
    static HttpResponse get(const std::string& url,
                           const std::map<std::string, std::string>& headers = {});

    // Simple POST request
    static HttpResponse post(const std::string& url,
                            const std::string& body,
                            const std::map<std::string, std::string>& headers = {},
                            long timeout_seconds = 300);

    // Streaming POST request (calls callback for each chunk as it arrives)
    static HttpResponse post_stream(const std::string& url,
                                   const std::string& body,
                                   StreamCallback stream_callback,
                                   const std::map<std::string, std::string>& headers = {},
                                   long timeout_seconds = 300);

    // Download file to disk with automatic retry and resume support
    static DownloadResult download_file(const std::string& url,
                                        const std::string& output_path,
                                        ProgressCallback callback = nullptr,
                                        const std::map<std::string, std::string>& headers = {},
                                        const DownloadOptions& options = DownloadOptions());

    // Check if URL is reachable
    static bool is_reachable(const std::string& url, int timeout_seconds = 5);

private:
    // Single download attempt, may resume from offset
    static DownloadResult download_attempt(const std::string& url,
                                           const std::string& output_path,
                                           size_t resume_from,
                                           ProgressCallback callback,
                                           const std::map<std::string, std::string>& headers,
                                           const DownloadOptions& options);
};

// Creates a throttled progress callback that prints at most once per second.
// The resume_offset is added to show total progress when resuming.
// Always returns true (never cancels) - for console output only.
inline ProgressCallback create_throttled_progress_callback(size_t resume_offset = 0) {
    auto last_print_time = std::make_shared<std::chrono::steady_clock::time_point>(
        std::chrono::steady_clock::now());
    auto printed_final = std::make_shared<bool>(false);
    auto offset = std::make_shared<size_t>(resume_offset);

    return [last_print_time, printed_final, offset](size_t current, size_t total) -> bool {
        size_t adjusted_current = current + *offset;
        size_t adjusted_total = total + *offset;

        if (adjusted_total > 0) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - *last_print_time);

            bool is_complete = (adjusted_current >= adjusted_total);

            if (is_complete && *printed_final) {
                return true;  // Continue (already printed final)
            }

            if (elapsed.count() >= 1000 || (is_complete && !*printed_final)) {
                int percent = is_complete ? 100 : static_cast<int>((adjusted_current * 100) / adjusted_total);
                double mb_current = adjusted_current / (1024.0 * 1024.0);
                double mb_total = adjusted_total / (1024.0 * 1024.0);
                std::cout << "  Progress: " << percent << "% ("
                         << std::fixed << std::setprecision(1)
                         << mb_current << "/" << mb_total << " MB)" << std::endl;
                *last_print_time = now;

                if (is_complete) {
                    *printed_final = true;
                }
            }
        }
        return true;  // Always continue (console callback never cancels)
    };
}

} // namespace utils
} // namespace lemon
