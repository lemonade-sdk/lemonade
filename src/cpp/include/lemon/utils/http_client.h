#pragma once

#include <string>
#include <map>
#include <functional>

namespace lemon {
namespace utils {

struct HttpResponse {
    int status_code;
    std::string body;
    std::map<std::string, std::string> headers;
};

using ProgressCallback = std::function<void(size_t downloaded, size_t total)>;

class HttpClient {
public:
    // Simple GET request
    static HttpResponse get(const std::string& url,
                           const std::map<std::string, std::string>& headers = {});
    
    // Simple POST request
    static HttpResponse post(const std::string& url,
                            const std::string& body,
                            const std::map<std::string, std::string>& headers = {});
    
    // Download file to disk
    static bool download_file(const std::string& url,
                             const std::string& output_path,
                             ProgressCallback callback = nullptr,
                             const std::map<std::string, std::string>& headers = {});
    
    // Check if URL is reachable
    static bool is_reachable(const std::string& url, int timeout_seconds = 5);
};

} // namespace utils
} // namespace lemon

