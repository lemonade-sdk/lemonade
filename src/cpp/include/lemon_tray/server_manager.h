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

namespace lemon_tray {

enum class LogLevel {
    DEBUG,
    INFO,
    WARNING,
    ERROR
};

class ServerManager {
public:
    ServerManager();
    ~ServerManager();
    
    // Server lifecycle
    bool start_server(
        const std::string& server_binary_path,
        int port,
        int ctx_size,
        const std::string& log_file
    );
    
    bool stop_server();
    bool restart_server();
    bool is_server_running() const;
    
    // Configuration
    void set_port(int port);
    void set_context_size(int ctx_size);
    bool set_log_level(LogLevel level);
    
    int get_port() const { return port_; }
    int get_context_size() const { return ctx_size_; }
    
    // API communication (returns JSON or throws exception)
    nlohmann::json get_health();
    nlohmann::json get_models();
    bool load_model(const std::string& model_name);
    bool unload_model();
    
    // Utility
    std::string get_base_url() const;
    
private:
    // Platform-specific process management
    bool spawn_process();
    bool terminate_process();
    bool is_process_alive() const;
    
    // HTTP communication
    std::string make_http_request(
        const std::string& endpoint,
        const std::string& method = "GET",
        const std::string& body = ""
    );
    
    // Member variables
    pid_t server_pid_;
    std::string server_binary_path_;
    std::string log_file_;
    int port_;
    int ctx_size_;
    std::atomic<bool> server_started_;
    
#ifdef _WIN32
    HANDLE process_handle_;
#endif
};

} // namespace lemon_tray

