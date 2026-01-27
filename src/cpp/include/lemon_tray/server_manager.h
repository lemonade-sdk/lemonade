#pragma once

#include <string>
#include <memory>
#include <atomic>
#include <nlohmann/json.hpp>

#ifdef _WIN32
// Critical: Define _WINSOCKAPI_ FIRST to prevent winsock.h from being included
#ifndef _WINSOCKAPI_
#define _WINSOCKAPI_
#endif

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
using pid_t = DWORD;

// Undefine Windows macros that conflict with our code
#ifdef ERROR
#undef ERROR
#endif
#else
#include <sys/types.h>
#endif

#include <httplib.h>

namespace lemon_tray {

enum class LogLevel {
    DEBUG,
    INFO,
    WARNING,
    ERROR
};

class ServerManager {
public:
    ServerManager(const std::string& host, int port);
    ~ServerManager();
    
    // Server lifecycle
    bool start_server(
        const std::string& server_binary_path,
        int port,
        const nlohmann::json& recipe_options,
        const std::string& log_file,
        const std::string& log_level,
        bool show_console,
        bool is_ephemeral,
        const std::string& host,
        int max_llm_models,
        int max_embedding_models,
        int max_reranking_models,
        int max_audio_models,
        int max_image_models,
        const std::string& extra_models_dir
    );
    
    bool stop_server();
    bool restart_server();
    bool is_server_running() const;
    
    // Configuration
    void set_port(int port);
    void set_context_size(int ctx_size);
    bool set_log_level(LogLevel level);
    
    int get_port() const { return port_; }
    std::string get_connection_host() const { return (host_ == "0.0.0.0") ? "localhost" : host_; }
    
    // API communication (returns JSON or throws exception)
    nlohmann::json get_health();
    nlohmann::json get_models();
    bool load_model(const std::string& model_name, const nlohmann::json& recipe_options=nlohmann::json::object(), bool save_options=false);
    bool unload_model();  // Unload all models
    bool unload_model(const std::string& model_name);  // Unload specific model
    
    // HTTP communication (public for custom requests)
    std::string make_http_request(
        const std::string& endpoint,
        const std::string& method = "GET",
        const std::string& body = "",
        int timeout_seconds = 5
    );
    httplib::Client make_http_client(int timeout_seconds, int connection_timeout);
    
private:
    // Platform-specific process management
    bool spawn_process();
    bool terminate_process();
    bool terminate_router_tree();  // Kills router and its children (but NOT parent tray app)
    bool is_process_alive() const;
    
#ifndef _WIN32
    // Linux-specific PID file management
    void write_pid_file();
    void remove_pid_file();
#endif
    
    // Member variables
    pid_t server_pid_;
    std::string server_binary_path_;
    std::string log_file_;
    std::string log_level_;
    std::string extra_models_dir_;
    std::string host_;
    std::string api_key_;
    int port_;
    int max_llm_models_;
    int max_embedding_models_;
    int max_reranking_models_;
    int max_audio_models_;
    int max_image_models_;
    nlohmann::json recipe_options_;
    bool show_console_;
    bool is_ephemeral_;  // Suppress output for ephemeral servers
    std::atomic<bool> server_started_;
    
#ifdef _WIN32
    HANDLE process_handle_;
#endif
};

} // namespace lemon_tray

