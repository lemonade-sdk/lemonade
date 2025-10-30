#pragma once

#include "platform/tray_interface.h"
#include "server_manager.h"
#include <memory>
#include <string>
#include <vector>

namespace lemon_tray {

struct AppConfig {
    int port = 8000;
    int ctx_size = 4096;
    std::string log_file;
    std::string log_level = "info";
    std::string server_binary;
    bool no_tray = false;
    bool show_help = false;
    bool show_version = false;
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
    void on_toggle_debug_logs();
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
    bool debug_logs_enabled_;
    bool should_exit_;
    
    // Version info
    std::string current_version_;
    std::string latest_version_;
};

} // namespace lemon_tray

