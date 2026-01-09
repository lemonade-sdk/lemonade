#include "lemon_tray/server_manager.h"
#include "lemon/version.h"
#include <iostream>
#include <sstream>
#include <fstream>
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
#include <cerrno>   // For errno and ECHILD
#endif

// Use cpp-httplib for HTTP client (must be after Windows headers on Windows)
// Note: Not using OpenSSL support since we only connect to localhost
#include <httplib.h>

namespace lemon_tray {

ServerManager::ServerManager()
    : server_pid_(0)
    , port_(8000)
    , ctx_size_(4096)
    , host_("localhost")
    , show_console_(false)
    , is_ephemeral_(false)
    , server_started_(false)
#ifdef _WIN32
    , process_handle_(nullptr)
#endif
{
}

ServerManager::~ServerManager() {
    // Only stop server if this instance actually started it
    // Don't clean up servers we're just querying (e.g., in status commands)
    if (server_started_ && server_pid_ > 0) {
        stop_server();
    }
}

bool ServerManager::start_server(
    const std::string& server_binary_path,
    int port,
    int ctx_size,
    const std::string& log_file,
    const std::string& log_level,
    const std::string& llamacpp_backend,
    bool show_console,
    bool is_ephemeral,
    const std::string& llamacpp_args,
    const std::string& host,
    int max_llm_models,
    int max_embedding_models,
    int max_reranking_models,
    int max_audio_models,
    const std::string& extra_models_dir)
{
    if (is_server_running()) {
        DEBUG_LOG(this, "Server is already running");
        return true;
    }

    server_binary_path_ = server_binary_path;
    port_ = port;
    ctx_size_ = ctx_size;
    max_llm_models_ = max_llm_models;
    max_embedding_models_ = max_embedding_models;
    max_reranking_models_ = max_reranking_models;
    max_audio_models_ = max_audio_models;
    log_file_ = log_file;
    log_level_ = log_level;
    llamacpp_backend_ = llamacpp_backend;
    show_console_ = show_console;
    is_ephemeral_ = is_ephemeral;
    llamacpp_args_ = llamacpp_args;
    extra_models_dir_ = extra_models_dir;
    host_ = host;

    const char* api_key_env = std::getenv("LEMONADE_API_KEY");
    api_key_ = api_key_env ? std::string(api_key_env) : "";
    
    if (!spawn_process()) {
        std::cerr << "Failed to spawn server process" << std::endl;
        return false;
    }
    
    // Step 1: Wait for server process to start (check health endpoint)
    DEBUG_LOG(this, "Waiting for server process to start...");
    DEBUG_LOG(this, "Will check health at: http://" << host_ << ":" << port_ << "/api/v1/health");
    
    bool process_started = false;
    for (int i = 0; i < 5; ++i) {  // Wait up to 5 seconds
        DEBUG_LOG(this, "Health check attempt " << (i+1) << "/5...");
        std::this_thread::sleep_for(std::chrono::seconds(1));
        try {
            DEBUG_LOG(this, "Making HTTP request...");
            auto health = get_health();
            
            server_started_ = true;
            
#ifndef _WIN32
            // Write PID file on Linux for efficient server discovery
            DEBUG_LOG(this, "About to write PID file (PID: " << server_pid_ << ", Port: " << port_ << ")");
            write_pid_file();
#endif
            
            DEBUG_LOG(this, "Server process is running!");
            process_started = true;
            break;  // Process is up, move to next step
        } catch (const std::exception& e) {
            DEBUG_LOG(this, "Health check failed: " << e.what());
        } catch (...) {
            DEBUG_LOG(this, "Health check failed with unknown error");
        }
    }
    
    if (!process_started) {
        std::cerr << "Server failed to start within timeout" << std::endl;
        stop_server();
        return false;
    }
    
    // Step 2: Quick check if server is ready (try models endpoint with short timeout)
    DEBUG_LOG(this, "Checking if server is ready...");
    try {
        // Use 1 second timeout for quick check
        make_http_request("/api/v1/models", "GET", "", 1);
        
        // Success! Server is ready immediately
        if (!is_ephemeral) {
            std::cout << "Lemonade Server v" << LEMON_VERSION_STRING << " started on port " << port_ << std::endl;
            // Display localhost for 0.0.0.0 since that's what users can actually visit in a browser
            std::string display_host = (host_ == "0.0.0.0") ? "localhost" : host_;
            std::cout << "API endpoint: http://" << display_host << ":" << port_ << "/api/v1" << std::endl;
            std::cout << "Connect your apps to the endpoint above." << std::endl;
            std::cout << "Documentation: https://lemonade-server.ai/" << std::endl;
        }
        
        server_started_ = true;
        
#ifndef _WIN32
        // Write PID file on Linux for efficient server discovery
        DEBUG_LOG(this, "About to write PID file (PID: " << server_pid_ << ", Port: " << port_ << ")");
        write_pid_file();
#endif
        
        return true;
        
    } catch (const std::exception& e) {
        DEBUG_LOG(this, "Quick check failed (expected on first run): " << e.what());
    }
    
    // Step 3: Server is initializing, wait for it
    std::cout << "Setting things up..." << std::endl;
    
    // Step 4: Poll models endpoint with longer timeout
    for (int i = 0; i < 10; ++i) {
        DEBUG_LOG(this, "Waiting for initialization... attempt " << (i+1) << "/10");
        try {
            // Use 10 second timeout for initialization wait
            make_http_request("/api/v1/models", "GET", "", 10);
            
            // Success! Server is ready
            if (!is_ephemeral) {
                std::cout << "Lemonade Server v" << LEMON_VERSION_STRING << " started on port " << port_ << std::endl;
                // Display localhost for 0.0.0.0 since that's what users can actually visit in a browser
                std::string display_host = (host_ == "0.0.0.0") ? "localhost" : host_;
                std::cout << "API endpoint: http://" << display_host << ":" << port_ << "/api/v1" << std::endl;
                std::cout << "Connect your apps to the endpoint above." << std::endl;
                std::cout << "Documentation: https://lemonade-server.ai/" << std::endl;
            }
            
            server_started_ = true;
            
#ifndef _WIN32
            // Write PID file on Linux for efficient server discovery
            DEBUG_LOG(this, "About to write PID file (PID: " << server_pid_ << ", Port: " << port_ << ")");
            write_pid_file();
#endif
            
            return true;
            
        } catch (const std::exception& e) {
            DEBUG_LOG(this, "Still initializing: " << e.what());
            // Don't sleep here - the 10 second timeout handles the wait
        }
    }
    
    std::cerr << "Server failed to become ready within timeout" << std::endl;
    stop_server();
    return false;
}

bool ServerManager::stop_server() {
    DEBUG_LOG(this, "stop_server() called, server_started_=" << server_started_ << ", server_pid_=" << server_pid_);
    
    if (!is_server_running()) {
        DEBUG_LOG(this, "Server not running, checking for orphaned children...");
        
        // Even if server appears not running, try to kill children if we have a PID
        // The router might have crashed/exited but children could still be alive
        if (server_pid_ != 0) {
            DEBUG_LOG(this, "Attempting to clean up process tree for PID " << server_pid_);
            terminate_router_tree();
        }
        
        server_started_ = false;
        server_pid_ = 0;
        
#ifdef _WIN32
        if (process_handle_) {
            CloseHandle(process_handle_);
            process_handle_ = nullptr;
        }
#else
        remove_pid_file();
#endif
        
        return true;
    }
    
    DEBUG_LOG(this, "Stopping server and children...");
    
    // Kill the entire process tree (router + children)
    // This does NOT kill the parent tray app (we might be running inside it!)
    terminate_router_tree();
    
    server_started_ = false;
    server_pid_ = 0;
    
#ifdef _WIN32
    if (process_handle_) {
        CloseHandle(process_handle_);
        process_handle_ = nullptr;
    }
#else
    // Remove PID file on Linux
    remove_pid_file();
#endif
    
    // Only print message for non-ephemeral servers
    if (!is_ephemeral_) {
        std::cout << "Server stopped successfully" << std::endl;
    }
    DEBUG_LOG(this, "Server stopped");
    return true;
}

bool ServerManager::restart_server() {
    stop_server();
    std::this_thread::sleep_for(std::chrono::seconds(1));
    return start_server(server_binary_path_, port_, ctx_size_, log_file_, log_level_, llamacpp_backend_, show_console_, false, llamacpp_args_, host_);
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

bool ServerManager::load_model(const std::string& model_name, bool save_options) {
    try {
        nlohmann::json load_req = nlohmann::json::object();
        load_req["model_name"] = model_name;

        if (save_options) {
            load_req["save_options"] = true;
            load_req["ctx_size"] = ctx_size_;  
            load_req["llamacpp_backend"] = llamacpp_backend_;  
            load_req["llamacpp_args"] = llamacpp_args_;  
        } 

        std::string body = load_req.dump();
        
        // 24 hour timeout - models can be 100GB+ and downloads may need many retries
        DEBUG_LOG(this, "Loading model...");
        DEBUG_LOG(this, "Request body: " << body);
        
        std::string response = make_http_request("/api/v1/load", "POST", body, 86400);
        DEBUG_LOG(this, "Load request succeeded");
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Exception loading model: " << e.what() << std::endl;
        return false;
    }
}

bool ServerManager::unload_model() {
    // Unload all models by passing empty string
    return unload_model("");
}

bool ServerManager::unload_model(const std::string& model_name) {
    try {
        std::string body;
        if (!model_name.empty()) {
            body = "{\"model_name\": \"" + model_name + "\"}";
        }
        
        // Unload can take time, so use 30 second timeout
        make_http_request("/api/v1/unload", "POST", body, 30);
        return true;
    } catch (const std::exception& e) {
        if (!model_name.empty()) {
            std::cerr << "Exception unloading model '" << model_name << "': " << e.what() << std::endl;
        } else {
            std::cerr << "Exception unloading models: " << e.what() << std::endl;
        }
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
    cmdline += " --host " + host_;
    cmdline += " --ctx-size " + std::to_string(ctx_size_);
    cmdline += " --llamacpp " + llamacpp_backend_;
    cmdline += " --log-level debug";  // Always use debug logging for router
    if (!llamacpp_args_.empty()) {
        cmdline += " --llamacpp-args \"" + llamacpp_args_ + "\"";
    }
    // Multi-model support
    cmdline += " --max-loaded-models " + std::to_string(max_llm_models_) + " " +
               std::to_string(max_embedding_models_) + " " + std::to_string(max_reranking_models_) + " " +
               std::to_string(max_audio_models_);
    // Extra models directory
    if (!extra_models_dir_.empty()) {
        cmdline += " --extra-models-dir \"" + extra_models_dir_ + "\"";
    }
    
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

bool ServerManager::terminate_router_tree() {
    // Windows implementation: Kill router and its children
    // This does NOT kill the parent tray app!
    
    DEBUG_LOG(this, "terminate_router_tree() called for PID " << server_pid_);
    
    std::vector<DWORD> child_pids;
    
    // 1. Find router's children (before killing router)
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe32;
        pe32.dwSize = sizeof(pe32);
        if (Process32FirstW(snapshot, &pe32)) {
            do {
                if (pe32.th32ParentProcessID == server_pid_) {
                    child_pids.push_back(pe32.th32ProcessID);
                    DEBUG_LOG(this, "Found child process: PID " << pe32.th32ProcessID);
                }
            } while (Process32NextW(snapshot, &pe32));
        }
        CloseHandle(snapshot);
    }
    
    DEBUG_LOG(this, "Found " << child_pids.size() << " child process(es)");
    
    // 2. Terminate router
    if (process_handle_) {
        DEBUG_LOG(this, "Terminating router (PID: " << server_pid_ << ")");
        TerminateProcess(process_handle_, 0);
        WaitForSingleObject(process_handle_, 5000);  // Wait up to 5 seconds
    }
    
    // 3. Terminate children
    for (DWORD child_pid : child_pids) {
        DEBUG_LOG(this, "Terminating child process (PID: " << child_pid << ")");
        HANDLE hChild = OpenProcess(PROCESS_TERMINATE, FALSE, child_pid);
        if (hChild) {
            TerminateProcess(hChild, 0);
            WaitForSingleObject(hChild, 5000);  // Wait up to 5 seconds
            CloseHandle(hChild);
        }
    }
    
    DEBUG_LOG(this, "terminate_router_tree() complete");
    
    return true;
}

#else  // Unix/Linux/macOS

bool ServerManager::spawn_process() {
    pid_t pid = fork();
    
    if (pid < 0) {
        std::cerr << "Fork failed" << std::endl;
        return false;
    }
    
    if (pid == 0) {
        // Child process - redirect stdout/stderr to log file if specified, or /dev/null if not
        if (!log_file_.empty()) {
            int log_fd = open(log_file_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (log_fd >= 0) {
                dup2(log_fd, STDOUT_FILENO);
                dup2(log_fd, STDERR_FILENO);
                close(log_fd);
            } else {
                std::cerr << "Failed to open log file: " << log_file_ << std::endl;
            }
        } else {
            // Redirect to /dev/null to suppress output (for ephemeral servers)
            int null_fd = open("/dev/null", O_WRONLY);
            if (null_fd >= 0) {
                dup2(null_fd, STDOUT_FILENO);
                dup2(null_fd, STDERR_FILENO);
                close(null_fd);
            }
        }
        
        std::vector<const char*> args;
        args.push_back(server_binary_path_.c_str());
        args.push_back("--port");
        std::string port_str = std::to_string(port_);
        args.push_back(port_str.c_str());
        args.push_back("--host");
        args.push_back(host_.c_str());
        args.push_back("--ctx-size");
        std::string ctx_str = std::to_string(ctx_size_);
        args.push_back(ctx_str.c_str());
        args.push_back("--llamacpp");
        args.push_back(llamacpp_backend_.c_str());
        args.push_back("--log-level");
        args.push_back("debug");  // Always use debug logging
        
        // Add llamacpp_args if present
        if (!llamacpp_args_.empty()) {
            args.push_back("--llamacpp-args");
            args.push_back(llamacpp_args_.c_str());
        }
        
        // Multi-model support
        args.push_back("--max-loaded-models");
        std::string max_llm_str = std::to_string(max_llm_models_);
        std::string max_emb_str = std::to_string(max_embedding_models_);
        std::string max_rer_str = std::to_string(max_reranking_models_);
        std::string max_aud_str = std::to_string(max_audio_models_);
        args.push_back(max_llm_str.c_str());
        args.push_back(max_emb_str.c_str());
        args.push_back(max_rer_str.c_str());
        args.push_back(max_aud_str.c_str());

        // Extra models directory
        if (!extra_models_dir_.empty()) {
            args.push_back("--extra-models-dir");
            args.push_back(extra_models_dir_.c_str());
        }

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
        
        // Wait for process to exit gracefully (up to 5 seconds)
        int status;
        for (int i = 0; i < 50; i++) {  // 50 * 100ms = 5 seconds
            pid_t result = waitpid(server_pid_, &status, WNOHANG);
            if (result > 0) {
                // Process exited gracefully
                return true;
            } else if (result < 0) {
                // Error - likely ECHILD (child already reaped by SIGCHLD handler)
                // This means the process is already gone
                if (errno == ECHILD) {
                    return true;
                }
                // Other error, break out
                break;
            }
            // result == 0 means still running, continue waiting
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        // If still alive after 5 seconds, force kill
        std::cerr << "[ServerManager] Process did not exit gracefully, forcing termination..." << std::endl;
        kill(server_pid_, SIGKILL);
        waitpid(server_pid_, &status, 0);  // Block until process is dead
        
        return true;
    }
    return false;
}

bool ServerManager::is_process_alive() const {
    if (server_pid_ <= 0) return false;
    
    // First check if process exists at all
    if (kill(server_pid_, 0) != 0) {
        return false;  // Process doesn't exist
    }
    
    // Check if it's a zombie by reading /proc/PID/stat
    // Format: PID (name) STATE ...
    // State can be: R (running), S (sleeping), D (disk sleep), Z (zombie), T (stopped), etc.
    std::string stat_path = "/proc/" + std::to_string(server_pid_) + "/stat";
    std::ifstream stat_file(stat_path);
    if (!stat_file) {
        return false;  // Can't read stat, assume dead
    }
    
    std::string line;
    std::getline(stat_file, line);
    
    // Find the state character (after the closing paren of the process name)
    size_t paren_pos = line.rfind(')');
    if (paren_pos != std::string::npos && paren_pos + 2 < line.length()) {
        char state = line[paren_pos + 2];
        // Return false if zombie - we consider zombies as "not alive" for our purposes
        return (state != 'Z');
    }
    
    // If we can't parse the state, assume alive to be safe
    return true;
}

bool ServerManager::terminate_router_tree() {
    // Linux implementation: Kill router and its children
    // This does NOT kill the parent tray app!
    
    DEBUG_LOG(this, "terminate_router_tree() called for PID " << server_pid_);
    
    if (server_pid_ <= 0) {
        DEBUG_LOG(this, "Invalid server_pid, returning");
        return false;
    }
    
    std::vector<pid_t> child_pids;
    
    // 1. Find router's children BEFORE killing router
    // (they get reparented to init if router dies first)
    std::string cmd = "pgrep -P " + std::to_string(server_pid_);
    FILE* pipe = popen(cmd.c_str(), "r");
    if (pipe) {
        char buffer[128];
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            pid_t child_pid = atoi(buffer);
            if (child_pid > 0) {
                child_pids.push_back(child_pid);
                DEBUG_LOG(this, "Found child process: PID " << child_pid);
            }
        }
        pclose(pipe);
    }
    
    DEBUG_LOG(this, "Found " << child_pids.size() << " child process(es)");
    
    // 2. Send SIGTERM to router
    DEBUG_LOG(this, "Sending SIGTERM to router (PID: " << server_pid_ << ")");
    kill(server_pid_, SIGTERM);
    
    // 3. Send SIGTERM to children
    for (pid_t child_pid : child_pids) {
        DEBUG_LOG(this, "Sending SIGTERM to child process (PID: " << child_pid << ")");
        kill(child_pid, SIGTERM);
    }
    
    // 4. Wait up to 5 seconds for graceful shutdown
    bool all_dead = false;
    for (int i = 0; i < 50; i++) {  // 50 * 100ms = 5 seconds
        bool router_alive = (kill(server_pid_, 0) == 0);
        bool any_child_alive = false;
        
        for (pid_t child_pid : child_pids) {
            if (kill(child_pid, 0) == 0) {
                any_child_alive = true;
                break;
            }
        }
        
        if (!router_alive && !any_child_alive) {
            all_dead = true;
            DEBUG_LOG(this, "All processes exited gracefully");
            break;
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    // 5. Force kill if still alive
    if (!all_dead) {
        DEBUG_LOG(this, "Timeout expired, sending SIGKILL");
        kill(server_pid_, SIGKILL);
        for (pid_t child_pid : child_pids) {
            kill(child_pid, SIGKILL);
        }
        
        // Wait for forced kill to complete
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    
    DEBUG_LOG(this, "terminate_router_tree() complete");
    
    return true;
}

void ServerManager::write_pid_file() {
    std::string pid_file_path = "/tmp/lemonade-router.pid";
    DEBUG_LOG(this, "write_pid_file() called - PID: " << server_pid_ << ", Port: " << port_);
    
    std::ofstream pid_file(pid_file_path);
    if (pid_file.is_open()) {
        pid_file << server_pid_ << "\n" << port_ << "\n";
        pid_file.close();
        DEBUG_LOG(this, "Wrote PID file: " << pid_file_path << " (PID: " << server_pid_ << ", Port: " << port_ << ")");
        std::cout << "[ServerManager] PID file created: " << pid_file_path << std::endl;
    } else {
        std::cerr << "[ServerManager] ERROR: Failed to open PID file for writing: " << pid_file_path << std::endl;
        std::cerr << "[ServerManager] Error: " << strerror(errno) << std::endl;
    }
}

void ServerManager::remove_pid_file() {
    std::string pid_file_path = "/tmp/lemonade-router.pid";
    if (remove(pid_file_path.c_str()) == 0) {
        DEBUG_LOG(this, "Removed PID file: " << pid_file_path);
    }
    // Silently ignore if file doesn't exist
}

#endif

std::string ServerManager::make_http_request(
    const std::string& endpoint,
    const std::string& method,
    const std::string& body,
    int timeout_seconds)
{

    // Use the configured host to connect to the server
    // Special case: 0.0.0.0 is a bind address, not a connect address - use 127.0.0.1 instead
    std::string connect_host = (host_ == "0.0.0.0") ? "127.0.0.1" : host_;
    httplib::Client cli(connect_host, port_);
    cli.set_connection_timeout(10, 0);  // 10 second connection timeout
    cli.set_read_timeout(timeout_seconds, 0);  // Configurable read timeout

    if (api_key_ != "") {
        cli.set_bearer_token_auth(api_key_);
    }
    
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
        std::string error_msg;
        switch (err) {
            case httplib::Error::Read:
                // Read error usually means server closed connection (shutdown, Ctrl+C, etc.)
                error_msg = "Server connection closed (server may have shut down)";
                break;
            case httplib::Error::Write:
                error_msg = "Connection write error";
                break;
            case httplib::Error::Connection:
                error_msg = "Failed to connect to server at " + connect_host + ":" + std::to_string(port_);
                break;
            case httplib::Error::SSLConnection:
                error_msg = "SSL connection error";
                break;
            case httplib::Error::SSLServerVerification:
                error_msg = "SSL server verification failed";
                break;
            case httplib::Error::Canceled:
                error_msg = "Request was canceled";
                break;
            default:
                error_msg = "HTTP request failed (error code: " + std::to_string(static_cast<int>(err)) + ")";
                break;
        }
        throw std::runtime_error(error_msg);
    }
    
    if (res->status != 200) {
        // Try to parse error message from response body
        std::string error_msg = "HTTP request failed with status: " + std::to_string(res->status);
        try {
            auto error_json = nlohmann::json::parse(res->body);
            if (error_json.contains("error")) {
                error_msg = error_json["error"].get<std::string>();
            } else if (error_json.contains("detail")) {
                error_msg = error_json["detail"].get<std::string>();
            }
        } catch (...) {
            // If parsing fails, just use the generic error with the response body
            if (!res->body.empty() && res->body.length() < 200) {
                error_msg += ": " + res->body;
            }
        }
        throw std::runtime_error(error_msg);
    }
    
    return res->body;
}

} // namespace lemon_tray

