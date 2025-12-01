#include <lemon/utils/http_client.h>
#include <curl/curl.h>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <iostream>
#include <thread>
#include <chrono>
#include <filesystem>

namespace fs = std::filesystem;

namespace lemon {
namespace utils {

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
};

static int progress_callback(void* clientp, curl_off_t dltotal, curl_off_t dlnow, 
                             curl_off_t ultotal, curl_off_t ulnow) {
    if (dltotal > 0) {
        ProgressData* data = static_cast<ProgressData*>(clientp);
        if (data && data->callback) {
            data->callback(dlnow, dltotal);
        }
    }
    return 0;
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
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
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
            std::cerr << "[HttpClient ERROR] Callback data is null!" << std::endl;
            return 0;
        }
        
        if (!(*(data->callback))(ptr, total_size)) {
            return 0; // Signal error to stop transfer
        }
        
        return total_size;
    } catch (const std::exception& e) {
        std::cerr << "[HttpClient ERROR] Exception in stream callback: " << e.what() << std::endl;
        return 0;
    } catch (...) {
        std::cerr << "[HttpClient ERROR] Unknown exception in stream callback" << std::endl;
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
        std::cerr << "[HttpClient ERROR] " << error << std::endl;
        curl_slist_free_all(header_list);
        curl_easy_cleanup(curl);
        throw std::runtime_error(error);
    }
    
    // Log if we got a non-OK CURL code but continue (normal for streaming)
    if (res != CURLE_OK) {
        std::cerr << "[HttpClient] Stream ended with: " << curl_easy_strerror(res) 
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
    FILE* fp = fopen(output_path.c_str(), mode);
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
    
    if (res != CURLE_OK) {
        bool retryable = false;
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
            default:
                retryable = false;
        }
        
        size_t current_file_size = 0;
        if (fs::exists(output_path)) {
            current_file_size = fs::file_size(output_path);
        }
        result.can_resume = retryable && (current_file_size > 0);
        
        std::ostringstream oss;
        oss << "Download failed: " << result.curl_error << " (CURL code: " << result.curl_code << ")";
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
        // HTTP 416 when resuming means the file is already complete
        if (result.http_code == 416 && resume_from > 0) {
            std::cout << "\n[Download] File already complete" << std::endl;
            result.success = true;
            result.bytes_downloaded = 0;
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
    
    size_t resume_offset = 0;
    if (options.resume_partial && fs::exists(output_path)) {
        resume_offset = fs::file_size(output_path);
        if (resume_offset > 0) {
            std::cout << "\n[Download] Found partial file (" 
                      << std::fixed << std::setprecision(1) 
                      << (resume_offset / (1024.0 * 1024.0)) 
                      << " MB), resuming..." << std::endl;
        }
    }
    
    for (int attempt = 0; attempt <= options.max_retries; ++attempt) {
        if (attempt > 0) {
            std::cout << "\n[Download] Retry " << attempt << "/" << options.max_retries 
                      << " after " << (retry_delay_ms / 1000.0) << "s..." << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(retry_delay_ms));
            
            // Exponential backoff (parentheses avoid Windows min/max macro)
            retry_delay_ms = (std::min)(retry_delay_ms * 2, options.max_retry_delay_ms);
            
            if (options.resume_partial && fs::exists(output_path)) {
                size_t new_offset = fs::file_size(output_path);
                if (new_offset > resume_offset) {
                    resume_offset = new_offset;
                    std::cout << "[Download] Resuming from " 
                              << std::fixed << std::setprecision(1) 
                              << (resume_offset / (1024.0 * 1024.0)) << " MB" << std::endl;
                }
            }
        }
        
        ProgressCallback adjusted_callback = nullptr;
        if (callback) {
            adjusted_callback = [callback, resume_offset](size_t current, size_t total) {
                callback(current, total);
            };
        }
        
        final_result = download_attempt(url, output_path, resume_offset, 
                                        adjusted_callback, headers, options);
        
        if (final_result.success) {
            return final_result;
        }
        
        if (!final_result.can_resume && attempt < options.max_retries) {
            std::cerr << "\n[Download] Error (attempt " << (attempt + 1) << "): " 
                      << final_result.error_message << std::endl;
            
            if (fs::exists(output_path)) {
                std::cerr << "[Download] Removing incomplete file for fresh retry..." << std::endl;
                fs::remove(output_path);
            }
            resume_offset = 0;
        } else if (final_result.can_resume) {
            std::cerr << "\n[Download] Connection interrupted (attempt " << (attempt + 1) << "): " 
                      << final_result.curl_error << std::endl;
        } else {
            break;
        }
    }
    
    std::ostringstream oss;
    oss << "Download failed after " << (options.max_retries + 1) << " attempts.\n";
    oss << "Last error: " << final_result.error_message;
    
    if (fs::exists(output_path)) {
        size_t partial_size = fs::file_size(output_path);
        if (partial_size > 0) {
            oss << "\n\nPartial file preserved: " << output_path;
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
