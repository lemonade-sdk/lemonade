#ifndef LEMONADE_CLIENT_H
#define LEMONADE_CLIENT_H

#include <string>
#include <vector>
#include <functional>
#include <cstdint>
#include <chrono>
#include <nlohmann/json.hpp>

// Forward declaration for httplib
namespace httplib {
    class Client;
}

namespace lemonade {

class HttpError : public std::runtime_error {
public:
    HttpError(int status, std::string body, const std::string& message);

    int status_code() const;
    const std::string& response_body() const;

private:
    int status_code_;
    std::string response_body_;
};

// Helper struct for streaming request state
struct StreamingRequestState {
    std::string last_file;
    int last_percent = -1;
    bool success = false;
    std::string error_message;
    bool total_size_printed = false;
    uint64_t last_file_size = 0;
    std::chrono::steady_clock::time_point file_start_time;
    uint64_t file_bytes_resumed = 0;
};

// Model information structure
struct ModelInfo {
    std::string id;
    std::string checkpoint;
    std::string recipe;
    bool downloaded = false;
    bool suggested = false;
    std::vector<std::string> labels;
    std::string download_url;
    std::string description;
};

// Recipe backend status structure
struct BackendStatus {
    std::string name;
    std::string state;  // "installed", "unsupported", etc.
    std::string version;
    std::string message;
    std::string action;
};

// Recipe status structure
struct RecipeStatus {
    std::string name;
    std::vector<BackendStatus> backends;
};

// Main CLI client class
class LemonadeClient {
public:
    LemonadeClient(const std::string& host, int port, const std::string& api_key);
    ~LemonadeClient();

    // Model management commands
    int list_models(bool show_all) const;
    int pull_model(const nlohmann::json& model_data);
    int delete_model(const std::string& model_name) const;
    int load_model(const std::string& model_name, const nlohmann::json& recipe_options, bool save_options = false) const;
    int unload_model(const std::string& model_name) const;
    nlohmann::json get_model_info(const std::string& model_name) const;
    int launch_model(const std::string& model_name, const nlohmann::json& recipe_options, const std::string& agent);

    // Status commands
    int status(int display_port = 0) const;
    std::vector<ModelInfo> get_models(bool show_all) const;

    // Recipe/backend commands
    int list_recipes() const;
    int install_backend(const std::string& recipe, const std::string& backend, bool force = false);
    int uninstall_backend(const std::string& recipe, const std::string& backend);

    // Utility (timeouts are in milliseconds)
    std::string make_request(const std::string& path, const std::string& method = "GET",
                             const std::string& body = "", const std::string& content_type = "",
                             int connection_timeout_ms = 30000, int read_timeout_ms = 30000) const;

    // Streaming request overload (timeouts are in milliseconds)
    bool make_request(const std::string& path, const std::string& method,
                      const std::string& body, const std::string& content_type,
                      std::function<void(const std::string& event_type, const std::string& event_data)> callback,
                      int connection_timeout_ms = 30000, int read_timeout_ms = 30000) const;

private:
    std::string host_;
    int port_;
    std::string api_key_;
    std::string normalize_host(const std::string& host) const;
    std::string get_base_url() const;
};

} // namespace lemonade

#endif // LEMONADE_CLIENT_H
