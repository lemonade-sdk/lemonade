// Tray menu building and handlers for lemonade-server CLI
// These are TrayApp methods extracted to a separate file for organization.

#include "lemon_server/tray_app.h"
#include "lemon_server/platform/windows_tray.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <set>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <unistd.h>
#include <signal.h>
#endif

namespace lemon_server {

// Helper macro for debug logging (matches tray_app.cpp)
#define DEBUG_LOG(app, msg) \
    if ((app)->config_.log_level == "debug") { \
        std::cout << "DEBUG: " << msg << std::endl; \
    }

// ============================================================
// Menu Building
// ============================================================

void TrayApp::build_menu() {
    if (!tray_) return;
    
    Menu menu = create_menu();
    tray_->set_menu(menu);
}

Menu TrayApp::create_menu() {
    Menu menu;
    
    // Open app - at the very top (only if Electron app is available on full installer)
    if (electron_app_path_.empty()) {
        // Try to find the Electron app if we haven't already
        const_cast<TrayApp*>(this)->find_electron_app();
    }
    if (!electron_app_path_.empty()) {
        menu.add_item(MenuItem::Action("Open app", [this]() { launch_electron_app(); }));
        menu.add_separator();
    }
    
    // Get loaded model once and cache it to avoid redundant health checks
    std::string loaded = is_loading_model_ ? "" : get_loaded_model();
    // Get all loaded models to display at top and for checkmarks
    std::vector<LoadedModelInfo> loaded_models = is_loading_model_ ? std::vector<LoadedModelInfo>() : get_all_loaded_models();
    
    // Build a set of loaded model names for quick lookup
    std::set<std::string> loaded_model_names;
    for (const auto& m : loaded_models) {
        loaded_model_names.insert(m.model_name);
    }
    
    // Status display - show all loaded models at the top
    if (is_loading_model_) {
        std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(loading_mutex_));
        menu.add_item(MenuItem::Action("Loading: " + loading_model_name_ + "...", nullptr, false));
    } else {
        if (!loaded_models.empty()) {
            // Show each loaded model with its type
            for (const auto& model : loaded_models) {
                std::string display_text = "Loaded: " + model.model_name;
                if (!model.type.empty() && model.type != "llm") {
                    display_text += " (" + model.type + ")";
                }
                menu.add_item(MenuItem::Action(display_text, nullptr, false));
            }
        } else {
            menu.add_item(MenuItem::Action("No models loaded", nullptr, false));
        }
    }
    
    // Unload Model submenu
    auto unload_submenu = std::make_shared<Menu>();
    if (loaded_models.empty()) {
        unload_submenu->add_item(MenuItem::Action(
            "No models loaded",
            nullptr,
            false
        ));
    } else {
        for (const auto& model : loaded_models) {
            // Display model name with type if not LLM
            std::string display_text = model.model_name;
            if (!model.type.empty() && model.type != "llm") {
                display_text += " (" + model.type + ")";
            }
            unload_submenu->add_item(MenuItem::Action(
                display_text,
                [this, model_name = model.model_name]() { on_unload_specific_model(model_name); }
            ));
        }
        
        // Add "Unload all" option if multiple models are loaded
        if (loaded_models.size() > 1) {
            unload_submenu->add_separator();
            unload_submenu->add_item(MenuItem::Action(
                "Unload all",
                [this]() { on_unload_model(); }
            ));
        }
    }
    menu.add_item(MenuItem::Submenu("Unload Model", unload_submenu));
    
    // Load Model submenu
    auto load_submenu = std::make_shared<Menu>();
    auto models = get_downloaded_models();
    if (models.empty()) {
        load_submenu->add_item(MenuItem::Action(
            "No models available: Use the Model Manager",
            nullptr,
            false
        ));
    } else {
        for (const auto& model : models) {
            // Check if this model is in the loaded models set
            bool is_loaded = loaded_model_names.count(model.id) > 0;
            load_submenu->add_item(MenuItem::Checkable(
                model.id,
                [this, model]() { on_load_model(model.id); },
                is_loaded
            ));
        }
    }
    menu.add_item(MenuItem::Submenu("Load Model", load_submenu));
    
    // Port submenu
    auto port_submenu = std::make_shared<Menu>();
    std::vector<int> ports = {8000, 8020, 8040, 8060, 8080, 9000};
    for (int port : ports) {
        bool is_current = (port == config_.port);
        port_submenu->add_item(MenuItem::Checkable(
            "Port " + std::to_string(port),
            [this, port]() { on_change_port(port); },
            is_current
        ));
    }
    menu.add_item(MenuItem::Submenu("Port", port_submenu));
    
    // Context Size submenu
    auto ctx_submenu = std::make_shared<Menu>();
    std::vector<std::pair<std::string, int>> ctx_sizes = {
        {"4K", 4096}, {"8K", 8192}, {"16K", 16384},
        {"32K", 32768}, {"64K", 65536}, {"128K", 131072}
    };
    for (const auto& [label, size] : ctx_sizes) {
        bool is_current = (size == config_.ctx_size);
        ctx_submenu->add_item(MenuItem::Checkable(
            "Context size " + label,
            [this, size = size]() { on_change_context_size(size); },
            is_current
        ));
    }
    menu.add_item(MenuItem::Submenu("Context Size", ctx_submenu));
    
    menu.add_separator();
    
    // Main menu items
    menu.add_item(MenuItem::Action("Documentation", [this]() { on_open_documentation(); }));
    menu.add_item(MenuItem::Action("Show Logs", [this]() { on_show_logs(); }));
    
    menu.add_separator();
    menu.add_item(MenuItem::Action("Quit Lemonade", [this]() { on_quit(); }));
    
    return menu;
}

// ============================================================
// Menu Action Handlers
// ============================================================

void TrayApp::on_load_model(const std::string& model_name) {
    // CRITICAL: Make a copy IMMEDIATELY since model_name is a reference that gets invalidated
    // when build_menu() destroys the old menu (which destroys the lambda that captured the model)
    std::string model_name_copy = model_name;
    
    // Don't start a new load if one is already in progress
    if (is_loading_model_) {
        show_notification("Model Loading", "A model is already being loaded. Please wait.");
        return;
    }
    
    // Set loading state with mutex protection
    {
        std::lock_guard<std::mutex> lock(loading_mutex_);
        is_loading_model_ = true;
        loading_model_name_ = model_name_copy;
    }
    
    // Immediately update menu to show loading state
    build_menu();
    
    // Launch background thread to perform the actual loading
    std::thread([this, model_name_copy]() {
        std::cout << "Loading model: " << model_name_copy << std::endl;
        bool success = server_manager_->load_model(model_name_copy);
        
        // Clear loading state
        {
            std::lock_guard<std::mutex> lock(loading_mutex_);
            is_loading_model_ = false;
            loading_model_name_.clear();
        }
        
        // Update menu to show new status
        build_menu();
        
        // Show notification
        if (success) {
            loaded_model_ = model_name_copy;
            show_notification("Model Loaded", "Successfully loaded " + model_name_copy);
        } else {
            show_notification("Load Failed", "Failed to load " + model_name_copy);
        }
    }).detach();
}

void TrayApp::on_unload_model() {
    // Don't allow unload while a model is loading
    if (is_loading_model_) {
        show_notification("Model Loading", "Please wait for the current model to finish loading.");
        return;
    }
    
    std::cout << "Unloading all models" << std::endl;
    if (server_manager_->unload_model()) {
        loaded_model_.clear();
        build_menu();
    }
}

void TrayApp::on_unload_specific_model(const std::string& model_name) {
    // Copy to avoid reference invalidation when menu is rebuilt
    std::string model_name_copy = model_name;
    
    // Don't allow unload while a model is loading
    if (is_loading_model_) {
        show_notification("Model Loading", "Please wait for the current model to finish loading.");
        return;
    }
    
    std::cout << "Unloading model: '" << model_name_copy << "'" << std::endl;
    std::cout.flush();
    
    // Launch background thread to perform the unload
    std::thread([this, model_name_copy]() {
        std::cout << "Background thread: Unloading model: '" << model_name_copy << "'" << std::endl;
        std::cout.flush();
        
        server_manager_->unload_model(model_name_copy);
        
        // Update menu to show new status
        build_menu();
    }).detach();
}

void TrayApp::on_change_port(int new_port) {
    std::cout << "Changing port to: " << new_port << std::endl;
    config_.port = new_port;
    server_manager_->set_port(new_port);
    build_menu();
    show_notification("Port Changed", "Lemonade Server is now running on port " + std::to_string(new_port));
}

void TrayApp::on_change_context_size(int new_ctx_size) {
    std::cout << "Changing context size to: " << new_ctx_size << std::endl;
    config_.ctx_size = new_ctx_size;
    server_manager_->set_context_size(new_ctx_size);
    build_menu();
    
    std::string label = (new_ctx_size >= 1024) 
        ? std::to_string(new_ctx_size / 1024) + "K"
        : std::to_string(new_ctx_size);
    show_notification("Context Size Changed", "Lemonade Server context size is now " + label);
}

void TrayApp::on_show_logs() {
    if (config_.log_file.empty()) {
        show_notification("Error", "No log file configured");
        return;
    }
    
#ifdef _WIN32
    // Close existing log viewer if any
    if (log_viewer_process_) {
        TerminateProcess(log_viewer_process_, 0);
        CloseHandle(log_viewer_process_);
        log_viewer_process_ = nullptr;
    }
    
    // Find lemonade-log-viewer.exe in the same directory as this executable
    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::string exeDir = exePath;
    size_t lastSlash = exeDir.find_last_of("\\/");
    if (lastSlash != std::string::npos) {
        exeDir = exeDir.substr(0, lastSlash);
    }
    
    std::string logViewerPath = exeDir + "\\lemonade-log-viewer.exe";
    std::string cmd = "\"" + logViewerPath + "\" \"" + config_.log_file + "\"";
    
    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    
    if (CreateProcessA(
        nullptr,
        const_cast<char*>(cmd.c_str()),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NEW_CONSOLE,
        nullptr,
        nullptr,
        &si,
        &pi))
    {
        log_viewer_process_ = pi.hProcess;
        CloseHandle(pi.hThread);
    } else {
        show_notification("Error", "Failed to open log viewer");
    }
#elif defined(__APPLE__)
    // Kill existing log viewer if any
    if (log_viewer_pid_ > 0) {
        kill(log_viewer_pid_, SIGTERM);
        log_viewer_pid_ = 0;
    }
    
    // Fork and open Terminal.app with tail command
    pid_t pid = fork();
    if (pid == 0) {
        // Child process
        std::string cmd = "osascript -e 'tell application \"Terminal\" to do script \"tail -f " + config_.log_file + "\"'";
        execl("/bin/sh", "sh", "-c", cmd.c_str(), nullptr);
        exit(0);
    } else if (pid > 0) {
        log_viewer_pid_ = pid;
    }
#else
    // Kill existing log viewer if any
    if (log_viewer_pid_ > 0) {
        kill(log_viewer_pid_, SIGTERM);
        log_viewer_pid_ = 0;
    }
    
    // Fork and open gnome-terminal or xterm
    pid_t pid = fork();
    if (pid == 0) {
        // Child process
        std::string cmd = "gnome-terminal -- tail -f '" + config_.log_file + "' || xterm -e tail -f '" + config_.log_file + "'";
        execl("/bin/sh", "sh", "-c", cmd.c_str(), nullptr);
        exit(0);
    } else if (pid > 0) {
        log_viewer_pid_ = pid;
    }
#endif
}

void TrayApp::on_open_documentation() {
    open_url("https://lemonade-server.ai/docs/");
}

void TrayApp::on_upgrade() {
    // TODO: Implement upgrade functionality
    std::cout << "Upgrade functionality not yet implemented" << std::endl;
}

void TrayApp::on_quit() {
    std::cout << "Quitting application..." << std::endl;
    shutdown();
}

// ============================================================
// Menu Helper Functions
// ============================================================

void TrayApp::show_notification(const std::string& title, const std::string& message) {
    if (tray_) {
        tray_->show_notification(title, message);
    }
}

std::string TrayApp::get_loaded_model() {
    try {
        auto health = server_manager_->get_health();
        
        // Check if model is loaded
        if (health.contains("model_loaded") && !health["model_loaded"].is_null()) {
            std::string loaded = health["model_loaded"].get<std::string>();
            if (!loaded.empty()) {
                return loaded;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Failed to get loaded model: " << e.what() << std::endl;
    }
    
    return "";  // No model loaded
}

std::vector<LoadedModelInfo> TrayApp::get_all_loaded_models() {
    std::vector<LoadedModelInfo> result;
    
    try {
        auto health = server_manager_->get_health();
        
        // Check for new multi-model format first
        if (health.contains("all_models_loaded") && health["all_models_loaded"].is_array()) {
            for (const auto& model : health["all_models_loaded"]) {
                LoadedModelInfo info;
                info.model_name = model.value("model_name", "");
                info.checkpoint = model.value("checkpoint", "");
                info.type = model.value("type", "llm");
                info.device = model.value("device", "");
                info.last_use = model.value("last_use", 0.0);
                info.backend_url = model.value("backend_url", "");
                
                if (!info.model_name.empty()) {
                    result.push_back(info);
                }
            }
        } else if (health.contains("model_loaded") && !health["model_loaded"].is_null()) {
            // Fall back to single model format
            std::string loaded = health["model_loaded"].get<std::string>();
            if (!loaded.empty()) {
                LoadedModelInfo info;
                info.model_name = loaded;
                info.checkpoint = health.value("checkpoint_loaded", "");
                info.type = "llm";
                result.push_back(info);
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Failed to get loaded models: " << e.what() << std::endl;
    }
    
    return result;
}

std::vector<ModelInfo> TrayApp::get_downloaded_models() {
    try {
        auto models_json = server_manager_->get_models();
        std::vector<ModelInfo> models;
        
        // Parse the models JSON response
        // Expected format: {"data": [{"id": "...", "checkpoint": "...", "recipe": "..."}], "object": "list"}
        if (models_json.contains("data") && models_json["data"].is_array()) {
            for (const auto& model : models_json["data"]) {
                ModelInfo info;
                info.id = model.value("id", "");
                info.checkpoint = model.value("checkpoint", "");
                info.recipe = model.value("recipe", "");
                
                if (!info.id.empty()) {
                    models.push_back(info);
                }
            }
        } else {
            DEBUG_LOG(this, "No 'data' array in models response");
        }
        
        return models;
    } catch (const std::exception& e) {
        std::cerr << "Failed to get models: " << e.what() << std::endl;
        return {};
    }
}

} // namespace lemon_server

