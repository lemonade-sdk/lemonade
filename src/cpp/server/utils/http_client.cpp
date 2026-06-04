#include <lemon/utils/http_client.h>
#include <lemon/utils/path_utils.h>
#include <lemon/utils/aixlog.hpp>
#include <curl/curl.h>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <iostream>
#include <thread>
#include <chrono>
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <memory>
#include <vector>
#include <mbedtls/md.h>

namespace fs = std::filesystem;

namespace lemon {
namespace utils {

std::atomic<long> HttpClient::default_timeout_seconds_{300};

namespace {

static std::string trim_copy(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n\"'");
    if (first == std::string::npos) {
        return "";
    }
    const auto last = value.find_last_not_of(" \t\r\n\"'");
    return value.substr(first, last - first + 1);
}

static std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

static bool is_hex_digest(const std::string& value, size_t expected_len) {
    if (value.size() != expected_len) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isxdigit(c) != 0;
    });
}

struct ExpectedHash {
    std::string algorithm;
    std::string value;

    bool present() const { return !algorithm.empty() && !value.empty(); }
};

struct HashCheckResult {
    bool ok = false;
    std::string actual;
    std::string error;
};

static ExpectedHash parse_expected_hash(const DownloadOptions& options) {
    std::string algorithm = lower_copy(trim_copy(options.expected_hash_algorithm));
    std::string value = lower_copy(trim_copy(options.expected_hash));

    const auto colon = value.find(':');
    if (colon != std::string::npos) {
        const std::string prefix = lower_copy(trim_copy(value.substr(0, colon)));
        if (!prefix.empty() && algorithm.empty()) {
            algorithm = prefix;
        }
        value = trim_copy(value.substr(colon + 1));
    }

    if (algorithm == "sha-256") algorithm = "sha256";
    if (algorithm == "sha-1") algorithm = "sha1";
    if (algorithm == "gitsha1" || algorithm == "git-sha") algorithm = "git-sha1";

    if (!algorithm.empty() &&
        algorithm != "sha256" &&
        algorithm != "sha1" &&
        algorithm != "git-sha1") {
        return {};
    }

    if (algorithm.empty()) {
        if (is_hex_digest(value, 64)) {
            algorithm = "sha256";
        } else if (is_hex_digest(value, 40)) {
            algorithm = "sha1";
        }
    }

    if ((algorithm == "sha256" && !is_hex_digest(value, 64)) ||
        ((algorithm == "sha1" || algorithm == "git-sha1") && !is_hex_digest(value, 40))) {
        return {};
    }

    return {algorithm, value};
}


static std::string bytes_to_hex(const unsigned char* bytes, size_t len) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (size_t i = 0; i < len; ++i) {
        oss << std::setw(2) << static_cast<unsigned int>(bytes[i]);
    }
    return oss.str();
}

static HashCheckResult digest_file_with_library(const fs::path& path,
                                                const ExpectedHash& expected,
                                                const std::string& git_blob_prefix) {
    HashCheckResult result;

    const mbedtls_md_type_t md_type =
        (expected.algorithm == "sha256") ? MBEDTLS_MD_SHA256 :
        (expected.algorithm == "sha1" || expected.algorithm == "git-sha1") ? MBEDTLS_MD_SHA1 :
        MBEDTLS_MD_NONE;

    if (md_type == MBEDTLS_MD_NONE) {
        result.error = "unsupported hash algorithm: " + expected.algorithm;
        return result;
    }

    const mbedtls_md_info_t* md_info = mbedtls_md_info_from_type(md_type);
    if (!md_info) {
        result.error = "mbedTLS digest algorithm is unavailable: " + expected.algorithm;
        return result;
    }

    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);

    auto cleanup = [&ctx]() {
        mbedtls_md_free(&ctx);
    };

    if (mbedtls_md_setup(&ctx, md_info, 0) != 0 ||
        mbedtls_md_starts(&ctx) != 0) {
        cleanup();
        result.error = "failed to initialize mbedTLS digest context";
        return result;
    }

    if (!git_blob_prefix.empty() &&
        mbedtls_md_update(&ctx,
                          reinterpret_cast<const unsigned char*>(git_blob_prefix.data()),
                          git_blob_prefix.size()) != 0) {
        cleanup();
        result.error = "failed to hash Git blob prefix with mbedTLS";
        return result;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        cleanup();
        result.error = "failed to open file for hash verification";
        return result;
    }

    constexpr size_t buffer_size = 1024 * 1024;
    std::vector<unsigned char> buffer(buffer_size);
    while (file.good()) {
        file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = file.gcount();
        if (count > 0 &&
            mbedtls_md_update(&ctx, buffer.data(), static_cast<size_t>(count)) != 0) {
            cleanup();
            result.error = "failed while hashing file with mbedTLS";
            return result;
        }
    }
    if (file.bad()) {
        cleanup();
        result.error = "failed while reading file for hash verification";
        return result;
    }

    unsigned char digest[MBEDTLS_MD_MAX_SIZE] = {};
    if (mbedtls_md_finish(&ctx, digest) != 0) {
        cleanup();
        result.error = "failed to finalize mbedTLS digest";
        return result;
    }

    const size_t digest_len = static_cast<size_t>(mbedtls_md_get_size(md_info));
    cleanup();

    result.actual = bytes_to_hex(digest, digest_len);
    result.ok = (lower_copy(result.actual) == expected.value);
    return result;
}

static HashCheckResult calculate_file_hash(const fs::path& path, const ExpectedHash& expected) {
    HashCheckResult result;
    if (!expected.present()) {
        result.ok = true;
        return result;
    }

    std::string git_blob_prefix;
    if (expected.algorithm == "git-sha1") {
        std::error_code ec;
        const auto size = fs::file_size(path, ec);
        if (ec) {
            result.error = "failed to get file size for git-sha1 verification: " + ec.message();
            return result;
        }
        git_blob_prefix = "blob " + std::to_string(size) + std::string(1, '\0');
    }

    result = digest_file_with_library(path, expected, git_blob_prefix);
    if (!result.ok && result.error.empty()) {
        result.error = "hash mismatch: expected " + expected.algorithm + ":" + expected.value +
                       ", got " + expected.algorithm + ":" + result.actual;
    }
    return result;
}

static HashCheckResult verify_file_hash(const fs::path& path, const ExpectedHash& expected) {
    auto result = calculate_file_hash(path, expected);
    if (!expected.present() || result.ok) {
        return result;
    }
    LOG(ERROR, "Download") << "Content verification failed for " << path.string()
                            << ": " << result.error << std::endl;
    return result;
}

} // namespace

// Callback for writing response data to string
static size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total_size = size * nmemb;
    std::string* str = static_cast<std::string*>(userp);
    str->append(static_cast<char*>(contents), total_size);
    return total_size;
}

// Callback for writing to file
static size_t write_file_callback(void* ptr, size_t size, size_t nmemb, void* stream) {
    size_t written = fwrite(ptr, size, nmemb, static_cast<FILE*>(stream));
    return written;
}

// Callback for download progress
struct ProgressData {
    ProgressCallback callback;
    bool cancelled = false;  // Set to true when callback returns false
};

static fs::path get_disk_space_probe_path(const fs::path& output_path) {
    fs::path probe_path = output_path.parent_path();
    if (!probe_path.empty()) {
        return probe_path;
    }

    std::error_code ec;
    fs::path current_path = fs::current_path(ec);
    if (!ec && !current_path.empty()) {
        return current_path;
    }

    return fs::path(".");
}

// CURL progress callback - returns non-zero to abort transfer
static int progress_callback(void* clientp, curl_off_t dltotal, curl_off_t dlnow,
                             curl_off_t ultotal, curl_off_t ulnow) {
    ProgressData* data = static_cast<ProgressData*>(clientp);
    if (!data) return 0;

    // Check if already cancelled
    if (data->cancelled) {
        return 1;  // Abort transfer
    }

    if (dltotal > 0 && data->callback) {
        // Call user callback - returns false to cancel
        if (!data->callback(dlnow, dltotal)) {
            data->cancelled = true;
            return 1;  // Abort transfer
        }
    }
    return 0;  // Continue transfer
}

HttpResponse HttpClient::get(const std::string& url,
                             const std::map<std::string, std::string>& headers) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("Failed to initialize CURL");
    }

    HttpResponse response;
    std::string response_body;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, default_timeout_seconds_.load());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "lemon.cpp/1.0");

    // Add custom headers
    struct curl_slist* header_list = nullptr;
    for (const auto& header : headers) {
        std::string header_str = header.first + ": " + header.second;
        header_list = curl_slist_append(header_list, header_str.c_str());
    }
    if (header_list) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    }

    CURLcode res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        std::string error = "CURL error: " + std::string(curl_easy_strerror(res));
        curl_slist_free_all(header_list);
        curl_easy_cleanup(curl);
        throw std::runtime_error(error);
    }

    long response_code;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
    response.status_code = static_cast<int>(response_code);
    response.body = response_body;

    curl_slist_free_all(header_list);
    curl_easy_cleanup(curl);

    return response;
}

HttpResponse HttpClient::post(const std::string& url,
                              const std::string& body,
                              const std::map<std::string, std::string>& headers,
                              long timeout_seconds) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("Failed to initialize CURL");
    }

    HttpResponse response;
    std::string response_body;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "lemon.cpp/1.0");

     // Add custom headers
    struct curl_slist* header_list = nullptr;
    header_list = curl_slist_append(header_list, "Content-Type: application/json");
    for (const auto& header : headers) {
        std::string header_str = header.first + ": " + header.second;
        header_list = curl_slist_append(header_list, header_str.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);

    CURLcode res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        std::string error = "CURL error: " + std::string(curl_easy_strerror(res));
        curl_slist_free_all(header_list);
        curl_easy_cleanup(curl);
        throw std::runtime_error(error);
    }

    long response_code;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
    response.status_code = static_cast<int>(response_code);
    response.body = response_body;

    curl_slist_free_all(header_list);
    curl_easy_cleanup(curl);

    return response;
}

HttpResponse HttpClient::post_multipart(const std::string& url,
                                         const std::vector<MultipartField>& fields,
                                         long timeout_seconds) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("Failed to initialize CURL");
    }

    HttpResponse response;
    std::string response_body;

    curl_mime* mime = curl_mime_init(curl);

    for (const auto& field : fields) {
        curl_mimepart* part = curl_mime_addpart(mime);
        curl_mime_name(part, field.name.c_str());
        curl_mime_data(part, field.data.c_str(), field.data.size());
        if (!field.filename.empty()) {
            curl_mime_filename(part, field.filename.c_str());
        }
        if (!field.content_type.empty()) {
            curl_mime_type(part, field.content_type.c_str());
        }
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "lemon.cpp/1.0");

    CURLcode res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        std::string error = "CURL error: " + std::string(curl_easy_strerror(res));
        curl_mime_free(mime);
        curl_easy_cleanup(curl);
        throw std::runtime_error(error);
    }

    long response_code;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
    response.status_code = static_cast<int>(response_code);
    response.body = response_body;

    curl_mime_free(mime);
    curl_easy_cleanup(curl);

    return response;
}

// Helper struct to pass stream callback through C interface
struct StreamCallbackData {
    StreamCallback* callback;
    std::string* buffer;
};

// Static C-style callback function
static size_t stream_write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    try {
        StreamCallbackData* data = static_cast<StreamCallbackData*>(userdata);
        size_t total_size = size * nmemb;

        if (!data || !data->callback || !*(data->callback)) {
            LOG(ERROR, "HttpClient") << "Callback data is null!" << std::endl;
            return 0;
        }

        if (!(*(data->callback))(ptr, total_size)) {
            return 0; // Signal error to stop transfer
        }

        return total_size;
    } catch (const std::exception& e) {
        LOG(ERROR, "HttpClient") << "Exception in stream callback: " << e.what() << std::endl;
        return 0;
    } catch (...) {
        LOG(ERROR, "HttpClient") << "Unknown exception in stream callback" << std::endl;
        return 0;
    }
}

HttpResponse HttpClient::post_stream(const std::string& url,
                                     const std::string& body,
                                     StreamCallback stream_callback,
                                     const std::map<std::string, std::string>& headers,
                                     long timeout_seconds) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("Failed to initialize CURL");
    }

    HttpResponse response;

    // Create callback data
    StreamCallbackData callback_data;
    callback_data.callback = &stream_callback;
    callback_data.buffer = nullptr;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stream_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &callback_data);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "lemon.cpp/1.0");

    // Add custom headers
    struct curl_slist* header_list = nullptr;
    header_list = curl_slist_append(header_list, "Content-Type: application/json");
    for (const auto& header : headers) {
        std::string header_str = header.first + ": " + header.second;
        header_list = curl_slist_append(header_list, header_str.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);

    CURLcode res = curl_easy_perform(curl);

    // Get response code before checking for errors
    long response_code;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
    response.status_code = static_cast<int>(response_code);

    // For streaming, CURLE_PARTIAL_FILE or CURLE_RECV_ERROR at the end is normal
    // (backend closes connection after sending all data)
    if (res != CURLE_OK && res != CURLE_PARTIAL_FILE && res != CURLE_RECV_ERROR) {
        std::string error = "CURL error: " + std::string(curl_easy_strerror(res));
        LOG(ERROR, "HttpClient") << "" << error << std::endl;
        curl_slist_free_all(header_list);
        curl_easy_cleanup(curl);
        throw std::runtime_error(error);
    }

    // Log if we got a non-OK CURL code but continue (normal for streaming)
    if (res != CURLE_OK) {
        LOG(ERROR, "HttpClient") << "Stream ended with: " << curl_easy_strerror(res)
                  << " (response code: " << response_code << ")" << std::endl;
    }

    curl_slist_free_all(header_list);
    curl_easy_cleanup(curl);

    return response;
}

DownloadResult HttpClient::download_attempt(const std::string& url,
                                            const std::string& output_path,
                                            size_t resume_from,
                                            ProgressCallback callback,
                                            const std::map<std::string, std::string>& headers,
                                            const DownloadOptions& options) {
    DownloadResult result;

    CURL* curl = curl_easy_init();
    if (!curl) {
        result.error_message = "Failed to initialize CURL";
        return result;
    }

    const char* mode = (resume_from > 0) ? "ab" : "wb";
    fs::path output_path_fs = path_from_utf8(output_path);
#ifdef _WIN32
    std::wstring wide_mode = (resume_from > 0) ? L"ab" : L"wb";
    FILE* fp = _wfopen(output_path_fs.c_str(), wide_mode.c_str());
#else
    FILE* fp = fopen(output_path.c_str(), mode);
#endif
    if (!fp) {
        result.error_message = "Failed to open file for writing: " + output_path;
        curl_easy_cleanup(curl);
        return result;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_file_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "lemon.cpp/1.0");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, static_cast<long>(options.connect_timeout));
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, static_cast<long>(options.low_speed_limit));
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, static_cast<long>(options.low_speed_time));

    if (resume_from > 0) {
        curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE, static_cast<curl_off_t>(resume_from));
    }

    ProgressData* prog_data = nullptr;
    if (callback) {
        prog_data = new ProgressData();
        prog_data->callback = callback;
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, prog_data);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    }

    // Add custom headers including authentication
    struct curl_slist* header_list = nullptr;
    for (const auto& header : headers) {
        std::string header_str = header.first + ": " + header.second;
        header_list = curl_slist_append(header_list, header_str.c_str());
    }
    if (header_list) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    }

    CURLcode res = curl_easy_perform(curl);

    // Check if download was cancelled by user callback
    bool was_cancelled = (prog_data && prog_data->cancelled);

    curl_off_t downloaded = 0;
    curl_off_t total = 0;
    curl_easy_getinfo(curl, CURLINFO_SIZE_DOWNLOAD_T, &downloaded);
    curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &total);

    result.bytes_downloaded = static_cast<size_t>(downloaded);
    result.total_bytes = (total > 0) ? static_cast<size_t>(total) : 0;

    fclose(fp);
    curl_slist_free_all(header_list);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.http_code);

    result.curl_code = static_cast<int>(res);
    result.curl_error = curl_easy_strerror(res);

    curl_easy_cleanup(curl);

    // Clean up progress data
    if (prog_data) {
        delete prog_data;
    }

    // Handle user cancellation
    if (was_cancelled || res == CURLE_ABORTED_BY_CALLBACK) {
        result.cancelled = true;
        result.error_message = "Download cancelled by user";
        result.can_resume = true;  // Partial file can be resumed later
        return result;
    }

    if (res != CURLE_OK) {
        bool retryable = false;
        bool disk_full = false;
        const fs::path disk_space_probe_path = get_disk_space_probe_path(output_path_fs);
        switch (res) {
            case CURLE_COULDNT_CONNECT:
            case CURLE_COULDNT_RESOLVE_HOST:
            case CURLE_COULDNT_RESOLVE_PROXY:
            case CURLE_OPERATION_TIMEDOUT:
            case CURLE_SEND_ERROR:
            case CURLE_RECV_ERROR:
            case CURLE_GOT_NOTHING:
            case CURLE_PARTIAL_FILE:
            case CURLE_SSL_CONNECT_ERROR:
                retryable = true;
                break;
            case CURLE_WRITE_ERROR: {
                // CURLE_WRITE_ERROR (23) typically means disk full.
                // Check available disk space to confirm.
                std::error_code ec;
                auto si = fs::space(disk_space_probe_path, ec);
                if (!ec && si.available < 1024 * 1024) {  // Less than 1 MB free
                    disk_full = true;
                }
                retryable = false;
                break;
            }
            default:
                retryable = false;
        }

        size_t current_file_size = 0;
        if (fs::exists(output_path_fs)) {
            current_file_size = fs::file_size(output_path_fs);
        }
        result.can_resume = retryable && (current_file_size > 0);
        result.disk_full = disk_full;

        std::ostringstream oss;
        if (disk_full) {
            oss << "Disk full: not enough space to complete download";
            std::error_code ec;
            auto si = fs::space(disk_space_probe_path, ec);
            if (!ec) {
                oss << " (" << std::fixed << std::setprecision(1)
                    << (si.available / (1024.0 * 1024.0)) << " MB free)";
            }
        } else {
            oss << "Download failed: " << result.curl_error << " (CURL code: " << result.curl_code << ")";
        }
        if (result.bytes_downloaded > 0) {
            oss << "\n  Downloaded " << (result.bytes_downloaded / (1024.0 * 1024.0)) << " MB before failure";
        }
        if (current_file_size > 0) {
            oss << "\n  Partial file size: " << (current_file_size / (1024.0 * 1024.0)) << " MB";
            if (result.can_resume) {
                oss << " (resumable)";
            }
        }
        result.error_message = oss.str();
        return result;
    }

    if (result.http_code >= 400) {
        // HTTP 416 (Range Not Satisfiable) when resuming - verify if file is actually complete
        if (result.http_code == 416 && resume_from > 0) {
            // Do a HEAD request to get the actual file size
            CURL* head_curl = curl_easy_init();
            if (head_curl) {
                curl_easy_setopt(head_curl, CURLOPT_URL, url.c_str());
                curl_easy_setopt(head_curl, CURLOPT_NOBODY, 1L);  // HEAD request
                curl_easy_setopt(head_curl, CURLOPT_FOLLOWLOCATION, 1L);
                curl_easy_setopt(head_curl, CURLOPT_TIMEOUT, 30L);

                // Add headers
                struct curl_slist* head_headers = nullptr;
                for (const auto& header : headers) {
                    std::string header_str = header.first + ": " + header.second;
                    head_headers = curl_slist_append(head_headers, header_str.c_str());
                }
                if (head_headers) {
                    curl_easy_setopt(head_curl, CURLOPT_HTTPHEADER, head_headers);
                }

                CURLcode head_res = curl_easy_perform(head_curl);

                if (head_res == CURLE_OK) {
                    curl_off_t remote_size = 0;
                    curl_easy_getinfo(head_curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &remote_size);

                    curl_slist_free_all(head_headers);
                    curl_easy_cleanup(head_curl);

                    if (remote_size > 0 && static_cast<size_t>(remote_size) <= resume_from) {
                        // Local file is >= remote size, file is complete
                        LOG(INFO, "Download") << " File verified complete (local: "
                                  << (resume_from / (1024.0 * 1024.0)) << " MB, remote: "
                                  << (remote_size / (1024.0 * 1024.0)) << " MB)" << std::endl;
                        result.success = true;
                        result.bytes_downloaded = 0;
                        return result;
                    } else {
                        // Local file is smaller than remote, or size unknown - corrupted or needs restart
                        std::ostringstream oss;
                        oss << "Resume failed - local file (" << (resume_from / (1024.0 * 1024.0))
                            << " MB) doesn't match remote size (" << (remote_size / (1024.0 * 1024.0)) << " MB)";
                        result.error_message = oss.str();
                        result.can_resume = false;  // Force fresh download
                        return result;
                    }
                }

                curl_slist_free_all(head_headers);
                curl_easy_cleanup(head_curl);
            }

            // HEAD request failed, treat 416 as needing restart
            result.error_message = "Resume failed (HTTP 416) - could not verify file size";
            result.can_resume = false;
            return result;
        }

        std::ostringstream oss;
        oss << "HTTP error " << result.http_code << " for URL: " << url;
        result.error_message = oss.str();
        result.can_resume = false;
        return result;
    }

    result.success = true;
    return result;
}

DownloadResult HttpClient::download_file(const std::string& url,
                                         const std::string& output_path,
                                         ProgressCallback callback,
                                         const std::map<std::string, std::string>& headers,
                                         const DownloadOptions& options) {
    DownloadResult final_result;
    int retry_delay_ms = options.initial_retry_delay_ms;
    const ExpectedHash expected_hash = parse_expected_hash(options);

    if (!options.expected_hash.empty() && !expected_hash.present()) {
        final_result.success = false;
        final_result.error_message = "Invalid or unsupported expected hash: " + options.expected_hash;
        return final_result;
    }

    // Use .partial extension for in-progress downloads
    std::string partial_path = output_path + ".partial";
    fs::path output_path_fs = path_from_utf8(output_path);
    fs::path partial_path_fs = path_from_utf8(partial_path);

    // If a verified final file exists next to a stale .partial file, trust the
    // verified final file and remove the stale partial.
    if (expected_hash.present() && fs::exists(output_path_fs) && fs::exists(partial_path_fs)) {
        auto hash_result = verify_file_hash(output_path_fs, expected_hash);
        if (hash_result.ok) {
            std::error_code remove_partial_ec;
            fs::remove(partial_path_fs, remove_partial_ec);
            final_result.success = true;
            final_result.bytes_downloaded = 0;
            LOG(INFO, "Download") << "File already exists and hash verified; removed stale partial: "
                                  << output_path << std::endl;
            return final_result;
        }

        std::error_code remove_output_ec;
        fs::remove(output_path_fs, remove_output_ec);
        if (remove_output_ec) {
            final_result.success = false;
            final_result.error_message = "Existing file failed verification and could not be removed: "
                                       + remove_output_ec.message();
            return final_result;
        }
    }

    // Check if final file already exists and is complete. When the caller
    // provided a content hash, the final path is only trusted if the hash
    // matches; otherwise remove it and force a fresh download.
    if (fs::exists(output_path_fs) && !fs::exists(partial_path_fs)) {
        if (expected_hash.present()) {
            auto hash_result = verify_file_hash(output_path_fs, expected_hash);
            if (hash_result.ok) {
                final_result.success = true;
                final_result.bytes_downloaded = 0;
                LOG(INFO, "Download") << "File already exists and hash verified: "
                                      << output_path << std::endl;
                return final_result;
            }

            LOG(WARNING, "Download") << "Existing file failed verification; removing for fresh download: "
                                     << output_path << std::endl;
            std::error_code remove_ec;
            fs::remove(output_path_fs, remove_ec);
            if (remove_ec) {
                final_result.success = false;
                final_result.error_message = "Existing file failed verification and could not be removed: "
                                           + remove_ec.message();
                return final_result;
            }
        } else {
            // Final file exists with no partial - consider it complete when no
            // stronger source-of-truth hash is available.
            final_result.success = true;
            final_result.bytes_downloaded = 0;
            LOG(INFO, "Download") << "File already exists: " << output_path << std::endl;
            return final_result;
        }
    }

    // Check for existing partial file to resume
    size_t resume_offset = 0;
    if (options.resume_partial && fs::exists(partial_path_fs)) {
        resume_offset = fs::file_size(partial_path_fs);
        if (resume_offset > 0) {
            LOG(INFO, "Download") << " Found partial file ("
                      << std::fixed << std::setprecision(1)
                      << (resume_offset / (1024.0 * 1024.0))
                      << " MB), resuming..." << std::endl;
        }
    }

    for (int attempt = 0; attempt <= options.max_retries; ++attempt) {
        if (attempt > 0) {
            LOG(INFO, "Download") << " Retry " << attempt << "/" << options.max_retries
                      << " after " << (retry_delay_ms / 1000.0) << "s..." << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(retry_delay_ms));

            // Exponential backoff (parentheses avoid Windows min/max macro)
            retry_delay_ms = (std::min)(retry_delay_ms * 2, options.max_retry_delay_ms);

            if (options.resume_partial && fs::exists(partial_path_fs)) {
                size_t new_offset = fs::file_size(partial_path_fs);
                if (new_offset > resume_offset) {
                    resume_offset = new_offset;
                    LOG(INFO, "Download") << "Resuming from "
                              << std::fixed << std::setprecision(1)
                              << (resume_offset / (1024.0 * 1024.0)) << " MB" << std::endl;
                }
            }
        }

        ProgressCallback adjusted_callback = nullptr;
        if (callback) {
            // Adjust progress to account for resume offset
            // curl reports: current = bytes downloaded this session, total = remaining bytes
            // We want to show: (resume_offset + current) / (resume_offset + total)
            adjusted_callback = [callback, resume_offset](size_t current, size_t total) -> bool {
                if (total > 0) {  // Only call when we have valid progress info
                    return callback(resume_offset + current, resume_offset + total);
                }
                return true;  // Continue if no progress info yet
            };
        }

        // Download to .partial file
        final_result = download_attempt(url, partial_path, resume_offset,
                                        adjusted_callback, headers, options);

        // If cancelled by user, return immediately without retrying
        if (final_result.cancelled) {
            LOG(INFO, "Download") << " Cancelled by user" << std::endl;
            return final_result;
        }

        // If disk is full, fail immediately — retrying will just waste bandwidth
        if (final_result.disk_full) {
            LOG(ERROR, "HttpClient") << "[Download] " << final_result.error_message << std::endl;
            return final_result;
        }

        if (final_result.success) {
            if (expected_hash.present()) {
                auto hash_result = verify_file_hash(partial_path_fs, expected_hash);
                if (!hash_result.ok) {
                    final_result.success = false;
                    final_result.can_resume = false;
                    final_result.error_message = "Download content verification failed for " + output_path +
                                                 ": " + hash_result.error;
                    std::error_code remove_ec;
                    fs::remove(partial_path_fs, remove_ec);
                    resume_offset = 0;

                    if (attempt < options.max_retries) {
                        LOG(ERROR, "HttpClient") << "[Download] " << final_result.error_message
                                                  << "; retrying from scratch" << std::endl;
                        continue;
                    }

                    break;
                }
                LOG(INFO, "Download") << "Hash verified for " << output_path << std::endl;
            }

            // Download complete - rename .partial to final path
            std::error_code ec;
            fs::rename(partial_path_fs, output_path_fs, ec);
            if (ec) {
                // Rename failed - try copy and delete
                fs::copy_file(partial_path_fs, output_path_fs, fs::copy_options::overwrite_existing, ec);
                if (!ec) {
                    fs::remove(partial_path_fs, ec);
                }
            }
            if (ec) {
                final_result.success = false;
                final_result.error_message = "Download succeeded but failed to rename file: " + ec.message();
            }
            return final_result;
        }

        // Don't retry permanent HTTP failures (4xx client errors).
        // 408 Request Timeout and 429 Too Many Requests are transient and still retried.
        bool is_permanent_4xx = (final_result.http_code >= 400 && final_result.http_code < 500
                                 && final_result.http_code != 408
                                 && final_result.http_code != 429);
        if (is_permanent_4xx) {
            LOG(ERROR, "HttpClient") << "[Download] " << final_result.error_message << std::endl;
            if (fs::exists(partial_path_fs)) {
                fs::remove(partial_path_fs);
            }
            break;
        }

        if (!final_result.can_resume && attempt < options.max_retries) {
            LOG(ERROR, "HttpClient") << "\n[Download] Error (attempt " << (attempt + 1) << "): "
                      << final_result.error_message << std::endl;

            if (fs::exists(partial_path_fs)) {
                LOG(WARNING, "HttpClient") << "[Download] Removing incomplete file for fresh retry..." << std::endl;
                fs::remove(partial_path_fs);
            }
            resume_offset = 0;
        } else if (final_result.can_resume) {
            LOG(WARNING, "HttpClient") << "\n[Download] Connection interrupted (attempt " << (attempt + 1) << "): "
                      << final_result.curl_error << std::endl;
        } else {
            break;
        }
    }

    std::ostringstream oss;
    oss << "Download failed after " << (options.max_retries + 1) << " attempts.\n";
    oss << "Last error: " << final_result.error_message;

    if (fs::exists(partial_path_fs)) {
        size_t partial_size = fs::file_size(partial_path_fs);
        if (partial_size > 0) {
            oss << "\n\nPartial file preserved: " << partial_path;
            oss << "\nPartial size: " << std::fixed << std::setprecision(1)
                << (partial_size / (1024.0 * 1024.0)) << " MB";
            oss << "\n\nRun the command again to resume from where it left off.";
        }
    }

    final_result.error_message = oss.str();
    return final_result;
}

bool HttpClient::is_reachable(const std::string& url, int timeout_seconds) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        return false;
    }

    std::string response_body;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "lemon.cpp/1.0");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);

    CURLcode res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        curl_easy_cleanup(curl);
        return false;
    }

    // Check HTTP status code
    long response_code;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
    curl_easy_cleanup(curl);

    return response_code == 200;
}

} // namespace utils
} // namespace lemon
