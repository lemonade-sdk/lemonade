#pragma once

#include <string>
#include <map>
#include <vector>
#include <functional>
#include <chrono>
#include <memory>
#include <iostream>
#include <iomanip>
#include <atomic>

namespace lemon {
namespace utils {

struct HttpResponse {
    int status_code = 0;
    std::string body;
    std::map<std::string, std::string> headers;

    // Transport status from libcurl. For non-streaming callers this remains
    // CURLE_OK/0 because transport errors are thrown. Streaming callers need the
    // original code so they can distinguish a clean [DONE] from an interrupted
    // backend connection that happened after HTTP headers were received.
    int curl_code = 0;
    std::string curl_error;
};

struct MultipartField {
    std::string name;
    std::string data;
    std::string filename;       // empty for text fields
    std::string content_type;   // empty for text fields
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
    bool disk_full = false;            // True if download failed due to insufficient disk space
    bool permanent = false;            // Non-recoverable failure (e.g. unsupported protocol, malformed URL); do not retry
};

// Progress callback returns bool: true = continue, false = cancel download
using ProgressCallback = std::function<bool(size_t downloaded, size_t total)>;
using StreamCallback = std::function<bool(const char* data, size_t length)>;

// Trust boundary for HTTP requests that may follow redirects. Selects which URL
// schemes are allowed for the initial request and for redirect targets, and
// whether redirects are followed at all.
enum class HttpSecurityPolicy {
    // External hosts (Hugging Face, GitHub, release CDNs). Require https for the
    // initial request and every redirect hop; bound the redirect chain. This is
    // the default so untrusted destinations are HTTPS-only unless a caller opts
    // out for a specific trust boundary.
    ExternalHttpsOnly,
    // Lemonade-managed backends reached over loopback http://127.0.0.1. Require
    // http and never follow redirects.
    TrustedLoopback,
    // User-configured plaintext endpoints that explicitly opted in via
    // allow_insecure_http. Permit http and https; bound the redirect chain.
    AllowInsecureHttp,
};

// Download configuration options
struct DownloadOptions {
    int max_retries = 5;              // Maximum retry attempts
    int initial_retry_delay_ms = 1000; // Initial delay between retries (doubles each time)
    int max_retry_delay_ms = 60000;   // Maximum delay between retries (1 minute)
    bool resume_partial = true;       // Resume partial downloads if possible
    int low_speed_limit = 0;       // Minimum bytes/sec before timeout (disabled — 0 = no limit)
    int low_speed_time = 0;        // Seconds below low_speed_limit before timeout (disabled)
    int connect_timeout = 30;         // Connection timeout in seconds
    int no_progress_timeout = 60;      // Seconds without byte progress before aborting (0 = disabled)
    bool range_retry_on_zero_byte_retry = true; // Retry empty failed attempts with Range: 0-
    bool force_initial_range_request = false;   // Force Range: 0- even on the first attempt

    // Optional content verification. expected_hash accepts plain hex or
    // prefixed values like "sha256:<hex>", "sha1:<hex>", or
    // "git-sha1:<hex>". git-sha1 verifies the Git blob object id, i.e.
    // SHA1("blob <size>\0" + file bytes), which is what Hugging Face uses
    // for non-LFS file ETags. SHA256 is used for LFS objects and release assets.
    std::string expected_hash;
    std::string expected_hash_algorithm;
};

class HttpClient {
public:
    static void set_default_timeout(long timeout_seconds) {
        default_timeout_seconds_ = timeout_seconds;
    }

    static long get_default_timeout() {
        return default_timeout_seconds_;
    }

    // Simple GET request. timeout_seconds=0 (default) uses default_timeout_seconds_.
    static HttpResponse get(const std::string& url,
                           const std::map<std::string, std::string>& headers = {},
                           long timeout_seconds = 0,
                           HttpSecurityPolicy policy = HttpSecurityPolicy::ExternalHttpsOnly);

    // Simple POST request
    static HttpResponse post(const std::string& url,
                            const std::string& body,
                            const std::map<std::string, std::string>& headers = {},
                            long timeout_seconds = 300);

    // Multipart form data POST request
    static HttpResponse post_multipart(const std::string& url,
                                       const std::vector<MultipartField>& fields,
                                       long timeout_seconds = 300);

    // Streaming POST request (calls callback for each chunk as it arrives).
    // on_status fires once, before the first chunk is delivered, so callers can
    // divert an error body instead of forwarding it as payload bytes.
    static HttpResponse post_stream(const std::string& url,
                                   const std::string& body,
                                   StreamCallback stream_callback,
                                   const std::map<std::string, std::string>& headers = {},
                                   long timeout_seconds = 300,
                                   std::function<void(int status_code)> on_status = nullptr);

    // Download file to disk with automatic retry and resume support
    static DownloadResult download_file(const std::string& url,
                                        const std::string& output_path,
                                        ProgressCallback callback = nullptr,
                                        const std::map<std::string, std::string>& headers = {},
                                        const DownloadOptions& options = DownloadOptions(),
                                        HttpSecurityPolicy policy = HttpSecurityPolicy::ExternalHttpsOnly);

    // Check if URL is reachable
    static bool is_reachable(const std::string& url, int timeout_seconds = 5);

private:
    static std::atomic<long> default_timeout_seconds_;

    // Single download attempt, may resume from offset
    static DownloadResult download_attempt(const std::string& url,
                                           const std::string& output_path,
                                           size_t resume_from,
                                           ProgressCallback callback,
                                           const std::map<std::string, std::string>& headers,
                                           const DownloadOptions& options,
                                           bool initial_range_request,
                                           HttpSecurityPolicy policy);
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
