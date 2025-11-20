#include <lemon/utils/http_client.h>
#include <curl/curl.h>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <iostream>

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
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);  // 5 minute timeout
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

bool HttpClient::download_file(const std::string& url,
                               const std::string& output_path,
                               ProgressCallback callback,
                               const std::map<std::string, std::string>& headers) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        return false;
    }
    
    FILE* fp = fopen(output_path.c_str(), "wb");
    if (!fp) {
        curl_easy_cleanup(curl);
        return false;
    }
    
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_file_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);  // No timeout for large downloads
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "lemon.cpp/1.0");
    
    // Progress tracking
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
    
    fclose(fp);
    curl_slist_free_all(header_list);
    
    // Check HTTP response code
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    
    curl_easy_cleanup(curl);
    
    // Clean up progress data
    if (prog_data) {
        delete prog_data;
    }
    
    if (res != CURLE_OK) {
        std::cerr << "CURL download error: " << curl_easy_strerror(res) << std::endl;
        return false;
    }
    
    // Check for HTTP errors (404, 403, 500, etc.)
    if (http_code >= 400) {
        std::cerr << "HTTP error: " << http_code << " for URL: " << url << std::endl;
        return false;
    }
    
    return true;
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

