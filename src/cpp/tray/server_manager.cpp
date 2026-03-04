// Include server_manager.h first - it has the correct Windows header order
#include "lemon_tray/server_manager.h"

#include "lemon/version.h"
#include "lemon/recipe_options.h"
#include "lemon/system_info.h"
#include "lemon/utils/path_utils.h"
#include <iostream>
#include <fstream>
#include <thread>
#include <chrono>
#include <lemon/utils/aixlog.hpp>

#ifdef _WIN32
#include <tlhelp32.h>
#else
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <fcntl.h>  // For open() and file flags
#include <cerrno>   // For errno and ECHILD
#endif

namespace lemon_tray {

ServerManager::ServerManager(const std::string& host, int port)
    : server_pid_(0)
    , host_(host)
    , port_(port)
    , show_console_(false)
    , is_ephemeral_(false)
    , server_started_(false)
#ifdef _WIN32
    , process_handle_(nullptr)
#endif
{
    const char* api_key_env = std::getenv("LEMONADE_API_KEY");
    api_key_ = api_key_env ? std::string(api_key_env) : "";
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
    const nlohmann::json& recipe_options,
    const std::string& log_file,
    const std::string& log_level,
    bool show_console,
    bool is_ephemeral,
    const std::string& host,
    int max_loaded_models,
    const std::string& extra_models_dir)
{
    if (is_server_running()) {
        LOG(DEBUG, "ServerManager") << "Server is already running" << std::endl;
        return true;
    }

    server_binary_path_ = server_binary_path;
    port_ = port;
    recipe_options_ = recipe_options;
    max_loaded_models_ = max_loaded_models;
    log_file_ = log_file;
    log_level_ = log_level;
    show_console_ = show_console;
    is_ephemeral_ = is_ephemeral;
    extra_models_dir_ = extra_models_dir;
    host_ = host;

    LOG(DEBUG, "ServerManager") << "Starting server listening at " << host_ << ":" << port << std::endl;

    if (!spawn_process()) {
        LOG(ERROR, "ServerManager") << "Failed to spawn server process" << std::endl;
        return false;
    }

    // Step 1: Wait for server process to start (check health endpoint)
    LOG(DEBUG, "ServerManager") << "Waiting for server process to start..." << std::endl;
    LOG(DEBUG, "ServerManager") << "Will check health at: http://" << get_connection_host() << ":" << port_ << "/api/v1/health" << std::endl;

    bool process_started = false;
    for (int i = 0; i < 5; ++i) {  // Wait up to 5 seconds
        LOG(DEBUG, "ServerManager") << "Health check attempt " << (i+1) << "/5..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(1));
        try {
            LOG(DEBUG, "ServerManager") << "Making HTTP request..." << std::endl;
            auto health = get_health();

            server_started_ = true;

#ifndef _WIN32
            // Write PID file on Linux for efficient server discovery
            LOG(DEBUG, "ServerManager") << "About to write PID file (PID: " << server_pid_ << ", Port: " << port_ << ")" << std::endl;
            write_pid_file();
#endif

            LOG(DEBUG, "ServerManager") << "Server process is running!" << std::endl;
            process_started = true;
            break;  // Process is up, move to next step
        } catch (const std::exception& e) {
            LOG(DEBUG, "ServerManager") << "Health check failed: " << e.what() << std::endl;
        } catch (...) {
            LOG(DEBUG, "ServerManager") << "Health check failed with unknown error" << std::endl;
        }
    }

    if (!process_started) {
        LOG(ERROR, "ServerManager") << "Server failed to start within timeout" << std::endl;
        stop_server();
        return false;
    }

    // Step 2: Quick check if server is ready (try models endpoint with short timeout)
    LOG(DEBUG, "ServerManager") << "Checking if server is ready..." << std::endl;
    try {
        // Use 1 second timeout for quick check
        make_http_request("/api/v1/models", "GET", "", 1);

        // Success! Server is ready immediately
        if (!is_ephemeral) {
            LOG(INFO, "ServerManager") << "Lemonade Server v" << LEMON_VERSION_STRING << " started on port " << port_ << std::endl;
            // Display localhost for 0.0.0.0 since that's what users can actually visit in a browser
            std::string display_host = get_connection_host();
            LOG(INFO, "ServerManager") << "API endpoint: http://" << display_host << ":" << port_ << "/api/v1" << std::endl;
            LOG(INFO, "ServerManager") << "Connect your apps to the endpoint above." << std::endl;
            LOG(INFO, "ServerManager") << "Documentation: https://lemonade-server.ai/" << std::endl;
        }

        return true;

    } catch (const std::exception& e) {
        LOG(DEBUG, "ServerManager") << "Quick check failed (expected on first run): " << e.what() << std::endl;
    }

    // Step 3: Server is initializing, wait for it
    LOG(INFO, "ServerManager") << "Setting things up..." << std::endl;

    // Step 4: Poll models endpoint with longer timeout
    for (int i = 0; i < 10; ++i) {
        LOG(DEBUG, "ServerManager") << "Waiting for initialization... attempt " << (i+1) << "/10" << std::endl;
        try {
            // Use 10 second timeout for initialization wait
            make_http_request("/api/v1/models", "GET", "", 10);

            // Success! Server is ready
            if (!is_ephemeral) {
                LOG(INFO, "ServerManager") << "Lemonade Server v" << LEMON_VERSION_STRING << " started on port " << port_ << std::endl;
                // Display localhost for 0.0.0.0 since that's what users can actually visit in a browser
                std::string display_host = get_connection_host();
                LOG(INFO, "ServerManager") << "API endpoint: http://" << display_host << ":" << port_ << "/api/v1" << std::endl;
                LOG(INFO, "ServerManager") << "Connect your apps to the endpoint above." << std::endl;
                LOG(INFO, "ServerManager") << "Documentation: https://lemonade-server.ai/" << std::endl;
            }

            server_started_ = true;
            return true;

        } catch (const std::exception& e) {
            LOG(DEBUG, "ServerManager") << "Still initializing: " << e.what() << std::endl;
            // Don't sleep here - the 10 second timeout handles the wait
        }
    }

    LOG(ERROR, "ServerManager") << "Server failed to become ready within timeout" << std::endl;
    stop_server();
    return false;
}

bool ServerManager::stop_server() {
    LOG(DEBUG, "ServerManager") << "stop_server() called, server_started_=" << server_started_ << ", server_pid_=" << server_pid_ << std::endl;

    if (!is_server_running()) {
        LOG(DEBUG, "ServerManager") << "Server not running, checking for orphaned children..." << std::endl;

        // Even if server appears not running, try to kill children if we have a PID
        // The router might have crashed/exited but children could still be alive
        if (server_pid_ != 0) {
            LOG(DEBUG, "ServerManager") << "Attempting to clean up process tree for PID " << server_pid_ << std::endl;
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

    LOG(DEBUG, "ServerManager") << "Stopping server and children..." << std::endl;

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
        LOG(INFO, "ServerManager") << "Server stopped successfully" << std::endl;
    }
    LOG(DEBUG, "ServerManager") << "Server stopped" << std::endl;
    return true;
}

bool ServerManager::restart_server() {
    stop_server();
    std::this_thread::sleep_for(std::chrono::seconds(1));
    return start_server(server_binary_path_, port_, recipe_options_, log_file_, log_level_, show_console_, false, host_, max_loaded_models_, extra_models_dir_);
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
    if (ctx_size != recipe_options_["ctx_size"]) {
        recipe_options_["ctx_size"] = ctx_size;
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

bool ServerManager::load_model(const std::string& model_name, const nlohmann::json& recipe_options, bool save_options) {
    try {
        nlohmann::json load_req = nlohmann::json::object();
        load_req["model_name"] = model_name;

        if (save_options) {
            load_req["save_options"] = true;
        }

        for (auto& [key, opt] : recipe_options.items()) {
            load_req[key] = opt;
        }

        std::string body = load_req.dump();

        // 24 hour timeout - models can be 100GB+ and downloads may need many retries
        LOG(DEBUG, "ServerManager") << "Loading model..." << std::endl;
        LOG(DEBUG, "ServerManager") << "Request body: " << body << std::endl;

        std::string response = make_http_request("/api/v1/load", "POST", body, 86400);
        LOG(DEBUG, "ServerManager") << "Load request succeeded" << std::endl;
        return true;
    } catch (const std::exception& e) {
        LOG(ERROR, "ServerManager") << "Exception loading model: " << e.what() << std::endl;
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
            LOG(ERROR, "ServerManager") << "Exception unloading model '" << model_name << "': " << e.what() << std::endl;
        } else {
            LOG(ERROR, "ServerManager") << "Exception unloading models: " << e.what() << std::endl;
        }
        return false;
    }
}

// Platform-specific implementations

#ifdef _WIN32

bool ServerManager::spawn_process() {
    // Build command line (server doesn't support --log-file, so we'll redirect stdout/stderr)
    std::string cmdline = "\"" + server_binary_path_ + "\"";
    cmdline += " --port " + std::to_string(port_);
    cmdline += " --host " + host_;
    cmdline += " --log-level " + log_level_;

    std::vector<std::string> recipe_cli = lemon::RecipeOptions::to_cli_options(recipe_options_);

    for (const auto& arg : recipe_cli) {
        if (arg.find(" ") != std::string::npos) {
            cmdline += " \"" + arg + "\"";
        } else {
            cmdline += " " + arg;
        }
    }

    // Multi-model support (only if not default)
    if (max_loaded_models_ != 1) {
        cmdline += " --max-loaded-models " + std::to_string(max_loaded_models_);
    }
    // Extra models directory
    if (!extra_models_dir_.empty()) {
        cmdline += " --extra-models-dir \"" + extra_models_dir_ + "\"";
    }

    LOG(DEBUG, "ServerManager") << "Starting server: " << cmdline << std::endl;

    STARTUPINFOA si = {};
    si.cb = sizeof(si);

    // Redirect stdout/stderr to log file if specified
    // Note: When show_console_ is true, the parent process will tail the log file
    HANDLE log_handle = INVALID_HANDLE_VALUE;
    if (!log_file_.empty()) {
        LOG(DEBUG, "ServerManager") << "Redirecting output to: " << log_file_ << std::endl;

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
            LOG(ERROR, "ServerManager") << "Failed to create log file: " << GetLastError() << std::endl;
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
        LOG(DEBUG, "ServerManager") << "Setting working directory to: " << working_dir << std::endl;
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
        LOG(ERROR, "ServerManager") << "CreateProcess failed: " << GetLastError() << std::endl;
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

    LOG(DEBUG, "ServerManager") << "terminate_router_tree() called for PID " << server_pid_ << std::endl;

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
                    LOG(DEBUG, "ServerManager") << "Found child process: PID " << pe32.th32ProcessID << std::endl;
                }
            } while (Process32NextW(snapshot, &pe32));
        }
        CloseHandle(snapshot);
    }

    LOG(DEBUG, "ServerManager") << "Found " << child_pids.size() << " child process(es)" << std::endl;

    // 2. Terminate router
    if (process_handle_) {
        LOG(DEBUG, "ServerManager") << "Terminating router (PID: " << server_pid_ << ")" << std::endl;
        TerminateProcess(process_handle_, 0);
        WaitForSingleObject(process_handle_, 5000);  // Wait up to 5 seconds
    }

    // 3. Terminate children
    for (DWORD child_pid : child_pids) {
        LOG(DEBUG, "ServerManager") << "Terminating child process (PID: " << child_pid << ")" << std::endl;
        HANDLE hChild = OpenProcess(PROCESS_TERMINATE, FALSE, child_pid);
        if (hChild) {
            TerminateProcess(hChild, 0);
            WaitForSingleObject(hChild, 5000);  // Wait up to 5 seconds
            CloseHandle(hChild);
        }
    }

    LOG(DEBUG, "ServerManager") << "terminate_router_tree() complete" << std::endl;

    return true;
}

#else  // Unix/Linux/macOS

bool ServerManager::spawn_process() {
    pid_t pid = fork();

    if (pid < 0) {
        LOG(ERROR, "ServerManager") << "Fork failed" << std::endl;
        return false;
    }

    if (pid == 0) {
        if (!log_file_.empty() && log_file_ != "-") {
            int log_fd = open(log_file_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (log_fd >= 0) {
                dup2(log_fd, STDOUT_FILENO);
                dup2(log_fd, STDERR_FILENO);
                close(log_fd);
            } else {
                LOG(ERROR, "ServerManager") << "Failed to open log file: " << log_file_ << std::endl;
            }
        } else if (log_file_.empty()) {
            int null_fd = open("/dev/null", O_WRONLY);
            if (null_fd >= 0) {
                dup2(null_fd, STDOUT_FILENO);
                dup2(null_fd, STDERR_FILENO);
                close(null_fd);
            }
        }
        // log_file_ == "-": keep stdout/stderr connected for systemd

        std::vector<const char*> args;
        args.push_back(server_binary_path_.c_str());

        args.push_back("--port");
        std::string port_str = std::to_string(port_);
        args.push_back(port_str.c_str());
        args.push_back("--host");
        args.push_back(host_.c_str());

        if (log_level_ != "info") {
            args.push_back("--log-level");
            args.push_back(log_level_.c_str());
        }

        std::vector<std::string> recipe_cli = lemon::RecipeOptions::to_cli_options(recipe_options_);

        for (const auto& arg : recipe_cli) {
            args.push_back(arg.c_str());
        }

        // Multi-model support (only if not default)
        std::string max_models_str;
        if (max_loaded_models_ != 1) {
            args.push_back("--max-loaded-models");
            max_models_str = std::to_string(max_loaded_models_);
            args.push_back(max_models_str.c_str());
        }

        // Extra models directory
        if (!extra_models_dir_.empty()) {
            args.push_back("--extra-models-dir");
            args.push_back(extra_models_dir_.c_str());
        }

        args.push_back(nullptr);

        execv(server_binary_path_.c_str(), const_cast<char**>(args.data()));

        // If execv returns, it failed
        LOG(ERROR, "ServerManager") << "execv failed" << std::endl;
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
        LOG(ERROR, "ServerManager") << "Process did not exit gracefully, forcing termination..." << std::endl;
        kill(server_pid_, SIGKILL);
        waitpid(server_pid_, &status, 0);  // Block until process is dead

        return true;
    }
    return false;
}


bool ServerManager::is_process_alive() const {
    if (server_pid_ <= 0) return false;

    // Send signal 0 to check if process exists (works on Mac and Linux)
    if (kill(server_pid_, 0) == 0) {
        // Process exists.

#ifdef __linux__
        // Linux: Check for Zombie state via /proc
        std::string stat_path = "/proc/" + std::to_string(server_pid_) + "/stat";
        std::ifstream stat_file(stat_path);
        if (stat_file) {
            std::string line;
            std::getline(stat_file, line);
            size_t paren_pos = line.rfind(')');
            if (paren_pos != std::string::npos && paren_pos + 2 < line.length()) {
                char state = line[paren_pos + 2];
                return (state != 'Z'); // Return true if not a Zombie
            }
        }
        return true; // Assume alive if we can't read stat
#elif defined(__APPLE__)
        // macOS: zombies still respond to kill(0) with success.
        // We can check waitpid with WNOHANG to see if it's already dead/zombie.
        int status;
        pid_t result = waitpid(server_pid_, &status, WNOHANG);
        if (result == 0) {
            return true; // Child is still running
        } else if (result == server_pid_) {
            return false; // Child has exited (we just reaped it)
        }
        // If result == -1, process might not be our child or error, rely on kill(0)
        return true;
#else
        return true;
#endif
    }
    return false; // kill failed, process dead
}

bool ServerManager::terminate_router_tree() {
    // Linux implementation: Kill router and its children
    // This does NOT kill the parent tray app!

    LOG(DEBUG, "ServerManager") << "terminate_router_tree() called for PID " << server_pid_ << std::endl;

    if (server_pid_ <= 0) {
        LOG(DEBUG, "ServerManager") << "Invalid server_pid, returning" << std::endl;
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
                LOG(DEBUG, "ServerManager") << "Found child process: PID " << child_pid << std::endl;
            }
        }
        pclose(pipe);
    }

    LOG(DEBUG, "ServerManager") << "Found " << child_pids.size() << " child process(es)" << std::endl;

    // 2. Send SIGTERM to router
    LOG(DEBUG, "ServerManager") << "Sending SIGTERM to router (PID: " << server_pid_ << ")" << std::endl;
    kill(server_pid_, SIGTERM);

    // 3. Send SIGTERM to children
    for (pid_t child_pid : child_pids) {
        LOG(DEBUG, "ServerManager") << "Sending SIGTERM to child process (PID: " << child_pid << ")" << std::endl;
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
            LOG(DEBUG, "ServerManager") << "All processes exited gracefully" << std::endl;
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // 5. Force kill if still alive
    if (!all_dead) {
        LOG(DEBUG, "ServerManager") << "Timeout expired, sending SIGKILL" << std::endl;
        kill(server_pid_, SIGKILL);
        for (pid_t child_pid : child_pids) {
            kill(child_pid, SIGKILL);
        }

        // Wait for forced kill to complete
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    LOG(DEBUG, "ServerManager") << "terminate_router_tree() complete" << std::endl;

    return true;
}

void ServerManager::write_pid_file() {
    std::string pid_file_path = lemon::utils::get_runtime_dir() + "/lemonade-router.pid";
    LOG(DEBUG, "ServerManager") << "write_pid_file() called - PID: " << server_pid_ << ", Port: " << port_ << std::endl;

    std::ofstream pid_file(pid_file_path);
    if (pid_file.is_open()) {
        pid_file << server_pid_ << "\n" << port_ << "\n";
        pid_file.close();
        LOG(DEBUG, "ServerManager") << "PID file created: " << pid_file_path << std::endl;
    } else {
        LOG(ERROR, "ServerManager") << "Failed to open PID file for writing: " << pid_file_path << std::endl;
        LOG(ERROR, "ServerManager") << "Error: " << strerror(errno) << std::endl;
    }
}

void ServerManager::remove_pid_file() {
    std::string pid_file_path = lemon::utils::get_runtime_dir() + "/lemonade-router.pid";
    if (remove(pid_file_path.c_str()) == 0) {
        LOG(DEBUG, "ServerManager") << "Removed PID file: " << pid_file_path << std::endl;
    }
}

#endif

httplib::Client ServerManager::make_http_client(int timeout_seconds, int connection_timeout) {
    // Use the configured host to connect to the server
    httplib::Client cli(get_connection_host(), port_);
    cli.set_connection_timeout(connection_timeout, 0);
    cli.set_read_timeout(timeout_seconds, 0);  // Configurable read timeout

    if (api_key_ != "") {
        cli.set_bearer_token_auth(api_key_);
    }

    return cli;
}

std::string ServerManager::make_http_request(
    const std::string& endpoint,
    const std::string& method,
    const std::string& body,
    int timeout_seconds)
{

    httplib::Client cli = make_http_client(timeout_seconds, 10); // 10 second connection timeout
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
                error_msg = "Failed to connect to server at " + get_connection_host() + ":" + std::to_string(port_);
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
