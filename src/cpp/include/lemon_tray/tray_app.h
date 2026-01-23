#pragma once

#include "platform/tray_interface.h"
#include "lemon/cli_parser.h"
#include "server_manager.h"
#include <memory>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>

namespace lemon_tray {

struct ModelInfo {
    std::string id;
    std::string checkpoint;
    std::string recipe;
};

// Information about a loaded model from the health endpoint
struct LoadedModelInfo {
    std::string model_name;
    std::string checkpoint;
    double last_use;
    std::string type;  // "llm", "embedding", or "reranking"
    std::string device;  // e.g., "gpu", "npu", "gpu npu"
    std::string backend_url;
};

class TrayApp {
public:
    TrayApp(const lemon::ServerConfig& server_config, const lemon::TrayConfig& tray_config);
    ~TrayApp();
    
    int run();
    void shutdown();  // Public method for signal handlers

#ifndef _WIN32
    // Signal handling infrastructure for Linux (self-pipe pattern)
    // Public so signal handler can access it
    static int signal_pipe_[2];
#endif
    
private:
    // Initialization
    bool find_server_binary();
    bool setup_logging();
    
    // Command implementations
    int execute_list_command();
    int execute_pull_command();
    int execute_delete_command();
    int execute_run_command();
    int execute_status_command();
    int execute_stop_command();
    
    // Helper functions for command execution
    bool is_server_running_on_port(int port);
    std::pair<int, int> get_server_info();  // Returns {pid, port}
    bool start_ephemeral_server(int port);
    
    // Server management
    bool start_server();
    void stop_server();
    
    // Menu building
    void build_menu();
    Menu create_menu();
    
    // Menu actions
    void on_load_model(const std::string& model_name);
    void on_unload_model();  // Unload all models (kept for backward compatibility)
    void on_unload_specific_model(const std::string& model_name);  // Unload specific model
    void on_change_port(int new_port);
    void on_change_context_size(int new_ctx_size);
    void on_show_logs();
    void on_open_documentation();
    void on_upgrade();
    void on_quit();
    
    // Helpers
    void open_url(const std::string& url);
    void launch_electron_app();
    bool find_electron_app();
    void show_notification(const std::string& title, const std::string& message);
    std::string get_loaded_model();
    std::vector<LoadedModelInfo> get_all_loaded_models();
    std::vector<ModelInfo> get_downloaded_models();
    
    // Member variables
    lemon::ServerConfig server_config_;
    lemon::TrayConfig tray_config_;
    std::string server_binary_;
    std::string log_file_;
    std::unique_ptr<TrayInterface> tray_;
    std::unique_ptr<ServerManager> server_manager_;
    std::string electron_app_path_;
    
    // State
    std::string loaded_model_;
    std::vector<ModelInfo> downloaded_models_;
    bool should_exit_;
    bool process_owns_server_ = false;
    
    // Model loading state
    std::atomic<bool> is_loading_model_{false};
    std::string loading_model_name_;
    std::mutex loading_mutex_;
    
    // Version info
    std::string current_version_;
    std::string latest_version_;
    
    // Log viewer process tracking
#ifdef _WIN32
    HANDLE log_viewer_process_ = nullptr;
#else
    pid_t log_viewer_pid_ = 0;
#endif

    // Electron app process tracking (for child process management and single-instance enforcement)
#ifdef _WIN32
    HANDLE electron_app_process_ = nullptr;
    HANDLE electron_job_object_ = nullptr;  // Job object to ensure child closes with parent
#else
    pid_t electron_app_pid_ = 0;  // Process ID of the Electron app (macOS/Linux)
#endif

    // Log tail thread for console output (when show_console is true)
    std::atomic<bool> stop_tail_thread_{false};
    std::thread log_tail_thread_;
    
    void tail_log_to_console();

#ifndef _WIN32
    // Signal monitor thread for Linux
    std::thread signal_monitor_thread_;
    std::atomic<bool> stop_signal_monitor_{false};
#endif
};

} // namespace lemon_tray

