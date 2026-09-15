#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace lemon {
namespace utils {

struct HttpResponse {
    int status_code = 0;
    std::string body;
    // Response headers, names lowercased. Populated by post(); other verbs
    // leave this empty.
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
    bool ranges_unsupported = false;   // Origin ignored Range; caller should use the single-stream path
    int parts_used = 1;                // Concurrent connections the transfer actually used
};

// Progress callback returns bool: true = continue, false = cancel download
using ProgressCallback = std::function<bool(size_t downloaded, size_t total)>;
using StreamCallback = std::function<bool(const char* data, size_t length)>;

// Trust boundary for outgoing HTTP requests. Selects which URL schemes are
// allowed for the initial request and, for request types that follow redirects,
// which schemes are allowed for redirect targets.
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
    // allow_insecure_http. Permit http and https; when redirects are enabled,
    // bound the redirect chain.
    AllowInsecureHttp,
};

// Concurrent connections a parallel download uses. Matches the count mainstream
// model downloaders settle on; the origin, not the client, is the limit well
// before this.
constexpr int kDefaultParallelParts = 16;

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

    // Concurrent ranged connections used for one file. 1 keeps the historical
    // single-stream transfer.
    // Parallelism is skipped, without failing, when the origin ignores Range, the
    // total size is unknown, the file is too small to split, or a download rate
    // limit is configured — a cap is enforced per connection, so N streams would
    // each receive the full cap.
    int parallel_parts = 1;
    // Floor on each connection's slice; the part count is reduced (possibly to 1,
    // which falls back to a single stream) rather than honoured blindly. Measured
    // against Hugging Face: at ~9 MB per slice 16 connections ran slower than one
    // (42.7 vs 49.6 MB/s), while at ~29 MB they ran faster (55.5 vs 46.3). The
    // cost being amortized is connection setup plus each stream's own ramp.
    size_t parallel_min_bytes_per_part = 32ull * 1024 * 1024;

    // Total size when the caller already knows it (Hugging Face manifests carry
    // it), which lets the parallel path skip a HEAD probe.
    size_t expected_total_bytes = 0;
};

class HttpClient {
public:
    // curl reads CURLOPT_TIMEOUT 0 as "no timeout", which collides with the
    // 0-means-default convention every request method here uses. A caller that
    // genuinely wants to block indefinitely passes this instead.
    static constexpr long kNoTimeout = -1;

    static void set_default_timeout(long timeout_seconds) {
        default_timeout_seconds_ = timeout_seconds;
    }

    static long get_default_timeout() {
        return default_timeout_seconds_;
    }

    // Global cap; when set, downloads are also serialized so concurrent
    // transfers cannot exceed it in aggregate.
    static void set_download_rate_limit(int64_t bytes_per_second) {
        download_rate_limit_bytes_per_second_ = bytes_per_second;
    }

    static int64_t get_download_rate_limit() {
        return download_rate_limit_bytes_per_second_.load();
    }

    // Simple GET request. timeout_seconds=0 (default) uses default_timeout_seconds_.
    static HttpResponse get(const std::string& url,
                           const std::map<std::string, std::string>& headers = {},
                           long timeout_seconds = 0,
                           HttpSecurityPolicy policy = HttpSecurityPolicy::ExternalHttpsOnly);

    // Simple POST request. Redirects are never followed.
    // timeout_seconds=0 uses default_timeout_seconds_, as in get(); pass
    // kNoTimeout to opt out deliberately.
    static HttpResponse post(
        const std::string& url,
        const std::string& body,
        const std::map<std::string, std::string>& headers = {},
        long timeout_seconds = 300,
        HttpSecurityPolicy policy = HttpSecurityPolicy::ExternalHttpsOnly,
        std::atomic<bool>* cancel_flag = nullptr);

    // Multipart form data POST request. Redirects are never followed.
    // timeout_seconds=0 uses default_timeout_seconds_.
    static HttpResponse post_multipart(
        const std::string& url,
        const std::vector<MultipartField>& fields,
        long timeout_seconds = 300,
        HttpSecurityPolicy policy = HttpSecurityPolicy::ExternalHttpsOnly);

    // Streaming POST request (calls callback for each chunk as it arrives).
    // on_status fires once, before the first chunk is delivered, so callers can
    // divert an error body instead of forwarding it as payload bytes. Redirects
    // are never followed.
    //
    // out_response_headers, when non-null, is filled with the response headers
    // (names lowercased) before on_status fires, so a caller deciding what to
    // send downstream can see them without waiting for the transfer to finish.
    //
    // timeout_seconds bounds upstream silence, not total duration: a total
    // timeout would cut off a long but healthy generation. 0 uses
    // default_timeout_seconds_, as in get().
    static HttpResponse post_stream(
        const std::string& url,
        const std::string& body,
        StreamCallback stream_callback,
        const std::map<std::string, std::string>& headers = {},
        long timeout_seconds = 300,
        std::function<void(int status_code)> on_status = nullptr,
        HttpSecurityPolicy policy = HttpSecurityPolicy::ExternalHttpsOnly,
        std::function<bool()> should_cancel = nullptr,
        std::map<std::string, std::string>* out_response_headers = nullptr);

    // Download file to disk with automatic retry and resume support
    static DownloadResult download_file(const std::string& url,
                                        const std::string& output_path,
                                        ProgressCallback callback = nullptr,
                                        const std::map<std::string, std::string>& headers = {},
                                        const DownloadOptions& options = DownloadOptions(),
                                        HttpSecurityPolicy policy = HttpSecurityPolicy::ExternalHttpsOnly);

    // Check if URL is reachable. Redirects are never followed.
    static bool is_reachable(
        const std::string& url,
        int timeout_seconds = 5,
        HttpSecurityPolicy policy = HttpSecurityPolicy::ExternalHttpsOnly);

private:
    static std::atomic<long> default_timeout_seconds_;
    static std::atomic<int64_t> download_rate_limit_bytes_per_second_;

    // Downloads one file over `parts` concurrent ranged connections into
    // output_path, which is pre-sized and written at absolute offsets. Per-part
    // progress is journalled beside it so an interrupted transfer resumes
    // without re-reading the (sparse) file size, which no longer reflects how
    // many bytes actually arrived.
    static DownloadResult download_parallel(const std::string& url,
                                            const std::string& output_path,
                                            size_t total_size,
                                            int parts,
                                            ProgressCallback callback,
                                            const std::map<std::string, std::string>& headers,
                                            const DownloadOptions& options,
                                            HttpSecurityPolicy policy);

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

// Bytes of output_path's in-progress download that have actually arrived.
// download_file's parallel path pre-sizes the .partial file, so from creation
// its size equals the final size and says nothing about progress; the per-part
// journal beside it is the only truthful source once one exists. Callers sizing
// a download or reporting progress MUST use this rather than fs::file_size.
size_t partial_bytes_on_disk(const std::string& output_path);

// The per-part progress journal beside an in-progress .partial file. Callers
// that delete or sweep .partial files must delete this too, or journals orphan.
std::string part_journal_path(const std::string& partial_path);

// Global flag: set from signal handler to cancel in-progress model downloads.
// Checked by the libcurl progress callback during transfer.
extern std::atomic<bool> g_download_cancelled;

} // namespace utils
} // namespace lemon
