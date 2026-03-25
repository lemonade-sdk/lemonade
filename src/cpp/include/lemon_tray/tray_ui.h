#pragma once

#include "lemon_tray/platform/tray_interface.h"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <memory>
#include <string>
#include <vector>
#include <set>
#include <atomic>
#include <mutex>
#include <thread>

namespace lemon_tray {

struct ModelInfo {
    std::string id;
    std::string checkpoint;
    std::string recipe;

    bool operator==(const ModelInfo& other) const {
        return id == other.id && checkpoint == other.checkpoint && recipe == other.recipe;
    }
};

struct LoadedModelInfo {
    std::string model_name;
    std::string checkpoint;
    double last_use;
    std::string type;
    std::string device;
    std::string backend_url;

    bool operator==(const LoadedModelInfo& other) const {
        // Exclude last_use — it changes on every request and would cause
        // spurious menu rebuilds.
        return model_name == other.model_name && checkpoint == other.checkpoint &&
               type == other.type && device == other.device &&
               backend_url == other.backend_url;
    }
};

class TrayUI {
public:
    TrayUI(int port, const std::string& host, bool silent = false);
    ~TrayUI();

    bool initialize();
    void run();   // Blocking event loop (main thread)
    void stop();  // Thread-safe, posts quit to event loop

private:
    // HTTP helpers (inline, using httplib::Client)
    std::string http_get(const std::string& endpoint);
    std::string http_post(const std::string& endpoint, const std::string& body = "");

    // Data fetchers
    std::pair<bool, std::vector<LoadedModelInfo>> fetch_server_state();
    std::vector<LoadedModelInfo> get_all_loaded_models();
    std::vector<ModelInfo> get_downloaded_models();

    // Menu
    void build_menu();
    void refresh_menu();
    Menu create_menu(const std::vector<LoadedModelInfo>& loaded_models,
                     const std::vector<ModelInfo>& available_models);
    bool menu_needs_refresh();

    // Menu actions
    void on_load_model(const std::string& model_name);
    void on_unload_model();
    void on_unload_specific_model(const std::string& model_name);
    void on_change_port(int new_port);
    void on_change_context_size(int new_ctx_size);
    void on_show_logs();
    void on_open_documentation();
    void on_quit();

    // App launch — uses lemonade:// protocol, falls back to web app
    void open_url(const std::string& url);
    bool try_open_lemonade_url(const std::string& lemonade_url);
    void open_desktop_app(const std::string& route = "");
    void open_web_app(const std::string& route = "");

    // Icon
    std::string find_icon_path();

    // Notifications
    void show_notification(const std::string& title, const std::string& message);

    // Connection helpers
    std::string get_connect_host() const;

    // State
    int port_;
    std::string host_;
    bool silent_;  // Suppress startup notification
    std::unique_ptr<TrayInterface> tray_;

    // Model loading state
    std::atomic<bool> is_loading_model_{false};
    std::string loading_model_name_;
    std::mutex loading_mutex_;

    // Menu refresh caching
    std::mutex state_mutex_;
    bool last_menu_server_reachable_ = false;
    std::vector<LoadedModelInfo> last_menu_loaded_models_;
    std::vector<ModelInfo> last_menu_available_models_;

    // Recipe options (for context size tracking)
    nlohmann::json recipe_options_;

    // Signal handling (non-Windows)
#ifndef _WIN32
    std::thread signal_monitor_thread_;
    std::atomic<bool> stop_signal_monitor_{false};
public:
    static int signal_pipe_[2];
#endif
};

} // namespace lemon_tray
