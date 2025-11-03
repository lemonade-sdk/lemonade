#pragma once

#include "platform/tray_interface.h"
#include "server_manager.h"
#include <memory>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>

namespace lemon_tray {

struct AppConfig {
    std::string command;  // No default - must be explicitly specified
    int port = 8000;
    int ctx_size = 4096;
    std::string log_file;
    std::string log_level = "info";  // Default to info, can be set to debug
    std::string server_binary;
    bool no_tray = false;
    bool show_help = false;
    bool show_version = false;
    std::string host = "localhost";
    std::string llamacpp_backend = "vulkan";  // Default to vulkan
    
    // For commands that take arguments
    std::vector<std::string> command_args;
};

struct ModelInfo {
    std::string id;
    std::string checkpoint;
    std::string recipe;
};

class TrayApp {
public:
    TrayApp(int argc, char* argv[]);
    ~TrayApp();
    
    int run();
    void shutdown();  // Public method for signal handlers
    
private:
    // Initialization
    void parse_arguments(int argc, char* argv[]);
    void print_usage();
    void print_version();
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
    bool wait_for_server_ready(int port, int timeout_seconds = 30);
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
    void on_unload_model();
    void on_change_port(int new_port);
    void on_change_context_size(int new_ctx_size);
    void on_show_logs();
    void on_open_documentation();
    void on_open_llm_chat();
    void on_open_model_manager();
    void on_upgrade();
    void on_quit();
    
    // Helpers
    void open_url(const std::string& url);
    void show_notification(const std::string& title, const std::string& message);
    std::string get_loaded_model();
    std::vector<ModelInfo> get_downloaded_models();
    
    // Member variables
    AppConfig config_;
    std::unique_ptr<TrayInterface> tray_;
    std::unique_ptr<ServerManager> server_manager_;
    
    // State
    std::string loaded_model_;
    std::vector<ModelInfo> downloaded_models_;
    bool should_exit_;
    
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

    // Log tail thread for console output (when show_console is true)
    std::atomic<bool> stop_tail_thread_{false};
    std::thread log_tail_thread_;
    
    void tail_log_to_console();
};

} // namespace lemon_tray

