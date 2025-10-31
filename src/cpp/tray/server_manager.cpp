#include "lemon_tray/server_manager.h"
#include <iostream>
#include <sstream>
#include <thread>
#include <chrono>

// Helper macro for debug logging
#define DEBUG_LOG(mgr, msg) \
    if ((mgr)->log_level_ == "debug") { \
        std::cout << "DEBUG: " << msg << std::endl; \
    }

#ifdef _WIN32
// Critical: Define _WINSOCKAPI_ FIRST to prevent winsock.h from being included
// This MUST come before any Windows headers
#ifndef _WINSOCKAPI_
#define _WINSOCKAPI_   // Prevent inclusion of winsock.h
#endif

// Now include winsock2.h first
#include <winsock2.h>
#include <ws2tcpip.h>

// Then define these and include other Windows headers
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>

// Undefine Windows macros that conflict with our enums
#ifdef ERROR
#undef ERROR
#endif
#else
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <fcntl.h>  // For open() and file flags
#endif

// Use cpp-httplib for HTTP client (must be after Windows headers on Windows)
// Note: Not using OpenSSL support since we only connect to localhost
#include <httplib.h>

namespace lemon_tray {

ServerManager::ServerManager()
    : server_pid_(0)
    , port_(8000)
    , ctx_size_(4096)
    , show_console_(false)
    , server_started_(false)
#ifdef _WIN32
    , process_handle_(nullptr)
#endif
{
}

ServerManager::~ServerManager() {
    stop_server();
}

bool ServerManager::start_server(
    const std::string& server_binary_path,
    int port,
    int ctx_size,
    const std::string& log_file,
    const std::string& log_level,
    bool show_console)
{
    if (is_server_running()) {
        DEBUG_LOG(this, "Server is already running");
        return true;
    }
    
    server_binary_path_ = server_binary_path;
    port_ = port;
    ctx_size_ = ctx_size;
    log_file_ = log_file;
    log_level_ = log_level;
    show_console_ = show_console;
    
    if (!spawn_process()) {
        std::cerr << "Failed to spawn server process" << std::endl;
        return false;
    }
    
    // Wait for server to be ready (check health endpoint)
    DEBUG_LOG(this, "Waiting for server to start...");
    DEBUG_LOG(this, "Will check health at: http://localhost:" << port_ << "/api/v1/health");
    
    for (int i = 0; i < 30; ++i) {  // Wait up to 30 seconds
        DEBUG_LOG(this, "Health check attempt " << (i+1) << "/30...");
        std::this_thread::sleep_for(std::chrono::seconds(1));
        try {
            DEBUG_LOG(this, "Making HTTP request...");
            auto health = get_health();
            DEBUG_LOG(this, "Health check succeeded!");
            std::cout << "Server started on port " << port_ << std::endl;
            server_started_ = true;
            return true;
        } catch (const std::exception& e) {
            DEBUG_LOG(this, "Health check failed: " << e.what());
        } catch (...) {
            DEBUG_LOG(this, "Health check failed with unknown error");
        }
    }
    
    std::cerr << "Server failed to start within timeout" << std::endl;
    stop_server();
    return false;
}

bool ServerManager::stop_server() {
    if (!is_server_running()) {
        return true;
    }
    
    DEBUG_LOG(this, "Stopping server...");
    
    // Try graceful shutdown first via API
    try {
        make_http_request("/api/v1/halt", "POST");
        std::this_thread::sleep_for(std::chrono::seconds(2));
    } catch (...) {
        // API call failed, proceed to force termination
    }
    
    // Force terminate if still running
    if (is_process_alive()) {
        terminate_process();
    }
    
    server_started_ = false;
    server_pid_ = 0;
    
#ifdef _WIN32
    if (process_handle_) {
        CloseHandle(process_handle_);
        process_handle_ = nullptr;
    }
#endif
    
    DEBUG_LOG(this, "Server stopped");
    return true;
}

bool ServerManager::restart_server() {
    stop_server();
    std::this_thread::sleep_for(std::chrono::seconds(1));
    return start_server(server_binary_path_, port_, ctx_size_, log_file_, log_level_);
}

bool ServerManager::is_server_running() const {
    return server_started_ && is_process_alive();
}

void ServerManager::set_port(int port) {
    if (port != port_) {
        port_ = port;
        if (is_server_running()) {
            restart_server();
        }
    }
}

void ServerManager::set_context_size(int ctx_size) {
    if (ctx_size != ctx_size_) {
        ctx_size_ = ctx_size;
        if (is_server_running()) {
            restart_server();
        }
    }
}

bool ServerManager::set_log_level(LogLevel level) {
    std::string level_str;
    switch (level) {
        case LogLevel::DEBUG: level_str = "debug"; break;
        case LogLevel::INFO: level_str = "info"; break;
        case LogLevel::WARNING: level_str = "warning"; break;
        case LogLevel::ERROR: level_str = "error"; break;
    }
    
    try {
        std::string body = "{\"level\": \"" + level_str + "\"}";
        make_http_request("/api/v1/log-level", "POST", body);
        return true;
    } catch (...) {
        return false;
    }
}

nlohmann::json ServerManager::get_health() {
    std::string response = make_http_request("/api/v1/health");
    return nlohmann::json::parse(response);
}

nlohmann::json ServerManager::get_models() {
    std::string response = make_http_request("/api/v1/models");
    return nlohmann::json::parse(response);
}

bool ServerManager::load_model(const std::string& model_name) {
    try {
        std::string body = "{\"model_name\": \"" + model_name + "\"}";
        
        // Model loading can take a long time, so use extended timeout
        DEBUG_LOG(this, "Loading model with extended timeout...");
        DEBUG_LOG(this, "Request body: " << body);
        
        httplib::Client cli("127.0.0.1", port_);
        cli.set_connection_timeout(10, 0);   // 10 second connection timeout
        cli.set_read_timeout(240, 0);        // 240 second (4 minute) read timeout for large models
        
        auto res = cli.Post("/api/v1/load", body, "application/json");
        
        if (!res) {
            DEBUG_LOG(this, "Load request connection error: " << static_cast<int>(res.error()));
            return false;
        }
        
        DEBUG_LOG(this, "Load request status: " << res->status);
        DEBUG_LOG(this, "Response body: " << res->body);
        
        if (res->status >= 200 && res->status < 300) {
            return true;
        }
        
        std::cerr << "Load model failed with status " << res->status << ": " << res->body << std::endl;
        return false;
    } catch (const std::exception& e) {
        std::cerr << "Exception loading model: " << e.what() << std::endl;
        return false;
    }
}

bool ServerManager::unload_model() {
    try {
        // Unload can also take time
        httplib::Client cli("127.0.0.1", port_);
        cli.set_connection_timeout(10, 0);
        cli.set_read_timeout(30, 0);  // 30 second timeout for unload
        
        auto res = cli.Post("/api/v1/unload", "", "application/json");
        
        if (!res) {
            DEBUG_LOG(this, "Unload request connection error: " << static_cast<int>(res.error()));
            return false;
        }
        
        return (res->status >= 200 && res->status < 300);
    } catch (const std::exception& e) {
        std::cerr << "Exception unloading model: " << e.what() << std::endl;
        return false;
    }
}

std::string ServerManager::get_base_url() const {
    return "http://127.0.0.1:" + std::to_string(port_);
}

// Platform-specific implementations

#ifdef _WIN32

bool ServerManager::spawn_process() {
    // Build command line (server doesn't support --log-file, so we'll redirect stdout/stderr)
    std::string cmdline = "\"" + server_binary_path_ + "\"";
    cmdline += " --port " + std::to_string(port_);
    cmdline += " --ctx-size " + std::to_string(ctx_size_);
    cmdline += " --log-level debug";  // Always use debug logging for router
    
    DEBUG_LOG(this, "Starting server: " << cmdline);
    
    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    
    // Redirect stdout/stderr to log file if specified
    // Note: When show_console_ is true, the parent process will tail the log file
    HANDLE log_handle = INVALID_HANDLE_VALUE;
    if (!log_file_.empty()) {
        DEBUG_LOG(this, "Redirecting output to: " << log_file_);
        
        SECURITY_ATTRIBUTES sa = {};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        sa.lpSecurityDescriptor = nullptr;
        
        log_handle = CreateFileA(
            log_file_.c_str(),
            GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            &sa,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );
        
        if (log_handle != INVALID_HANDLE_VALUE) {
            si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
            si.hStdOutput = log_handle;
            si.hStdError = log_handle;
            si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
            si.wShowWindow = SW_HIDE;
        } else {
            std::cerr << "Failed to create log file: " << GetLastError() << std::endl;
            si.dwFlags = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;
        }
    } else {
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
    }
    
    PROCESS_INFORMATION pi = {};
    
    // Get the directory containing the server executable to use as working directory
    // Resources are now in bin/resources/, so working dir should be bin/
    std::string working_dir;
    size_t last_slash = server_binary_path_.find_last_of("/\\");
    if (last_slash != std::string::npos) {
        working_dir = server_binary_path_.substr(0, last_slash);
        DEBUG_LOG(this, "Setting working directory to: " << working_dir);
    }
    
    // Create process
    if (!CreateProcessA(
        nullptr,
        const_cast<char*>(cmdline.c_str()),
        nullptr,
        nullptr,
        TRUE,  // Inherit handles so log file redirection works
        show_console_ ? 0 : CREATE_NO_WINDOW,  // Show console if requested
        nullptr,
        working_dir.empty() ? nullptr : working_dir.c_str(),  // Set working directory
        &si,
        &pi))
    {
        std::cerr << "CreateProcess failed: " << GetLastError() << std::endl;
        if (log_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(log_handle);
        }
        return false;
    }
    
    process_handle_ = pi.hProcess;
    server_pid_ = pi.dwProcessId;
    CloseHandle(pi.hThread);
    
    // Close the log file handle in parent process (child has its own copy)
    if (log_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(log_handle);
    }
    
    return true;
}

bool ServerManager::terminate_process() {
    if (process_handle_) {
        TerminateProcess(process_handle_, 1);
        WaitForSingleObject(process_handle_, 5000);  // Wait up to 5 seconds
        return true;
    }
    return false;
}

bool ServerManager::is_process_alive() const {
    if (!process_handle_) return false;
    
    DWORD exit_code;
    if (GetExitCodeProcess(process_handle_, &exit_code)) {
        return exit_code == STILL_ACTIVE;
    }
    return false;
}

#else  // Unix/Linux/macOS

bool ServerManager::spawn_process() {
    pid_t pid = fork();
    
    if (pid < 0) {
        std::cerr << "Fork failed" << std::endl;
        return false;
    }
    
    if (pid == 0) {
        // Child process - redirect stdout/stderr to log file if specified
        if (!log_file_.empty()) {
            int log_fd = open(log_file_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (log_fd >= 0) {
                dup2(log_fd, STDOUT_FILENO);
                dup2(log_fd, STDERR_FILENO);
                close(log_fd);
            } else {
                std::cerr << "Failed to open log file: " << log_file_ << std::endl;
            }
        }
        
        std::vector<const char*> args;
        args.push_back(server_binary_path_.c_str());
        args.push_back("--port");
        std::string port_str = std::to_string(port_);
        args.push_back(port_str.c_str());
        args.push_back("--ctx-size");
        std::string ctx_str = std::to_string(ctx_size_);
        args.push_back(ctx_str.c_str());
        args.push_back("--log-level");
        args.push_back("debug");  // Always use debug logging
        args.push_back(nullptr);
        
        execv(server_binary_path_.c_str(), const_cast<char**>(args.data()));
        
        // If execv returns, it failed
        std::cerr << "execv failed" << std::endl;
        exit(1);
    }
    
    // Parent process
    server_pid_ = pid;
    return true;
}

bool ServerManager::terminate_process() {
    if (server_pid_ > 0) {
        kill(server_pid_, SIGTERM);
        
        // Wait for process to exit
        int status;
        waitpid(server_pid_, &status, WNOHANG);
        
        return true;
    }
    return false;
}

bool ServerManager::is_process_alive() const {
    if (server_pid_ <= 0) return false;
    
    // Send signal 0 to check if process exists
    return kill(server_pid_, 0) == 0;
}

#endif

std::string ServerManager::make_http_request(
    const std::string& endpoint,
    const std::string& method,
    const std::string& body,
    int timeout_seconds)
{
    // Debug logging removed - too verbose for normal operations
    
    // Use 127.0.0.1 instead of "localhost" to avoid IPv6/IPv4 resolution issues on Windows
    httplib::Client cli("127.0.0.1", port_);
    cli.set_connection_timeout(10, 0);  // 10 second connection timeout
    cli.set_read_timeout(timeout_seconds, 0);  // Configurable read timeout
    
    httplib::Result res;
    
    if (method == "GET") {
        res = cli.Get(endpoint.c_str());
    } else if (method == "POST") {
        res = cli.Post(endpoint.c_str(), body, "application/json");
    } else {
        throw std::runtime_error("Unsupported HTTP method: " + method);
    }
    
    if (!res) {
        auto err = res.error();
        throw std::runtime_error("HTTP request failed: connection error");
    }
    
    if (res->status != 200) {
        throw std::runtime_error("HTTP request failed with status: " + std::to_string(res->status));
    }
    
    return res->body;
}

} // namespace lemon_tray

