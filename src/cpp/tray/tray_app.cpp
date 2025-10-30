#include "lemon_tray/tray_app.h"
#include "lemon_tray/platform/windows_tray.h"  // For set_menu_update_callback
#include <iostream>
#include <filesystem>
#include <algorithm>
#include <thread>
#include <chrono>
#include <csignal>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <cstdlib>
#include <unistd.h>  // for readlink
#endif

namespace fs = std::filesystem;

namespace lemon_tray {

// Global pointer to the current TrayApp instance for signal handling
static TrayApp* g_tray_app_instance = nullptr;

#ifdef _WIN32
// Windows Ctrl+C handler
BOOL WINAPI console_ctrl_handler(DWORD ctrl_type) {
    if (ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_CLOSE_EVENT || ctrl_type == CTRL_BREAK_EVENT) {
        std::cout << "\nReceived interrupt signal, shutting down gracefully..." << std::endl;
        
        if (g_tray_app_instance) {
            g_tray_app_instance->shutdown();
        }
        
        return TRUE;  // We handled it
    }
    return FALSE;
}
#else
// Unix signal handler
void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        std::cout << "\nReceived interrupt signal, shutting down gracefully..." << std::endl;
        
        if (g_tray_app_instance) {
            g_tray_app_instance->shutdown();
        }
        
        exit(0);
    }
}
#endif

TrayApp::TrayApp(int argc, char* argv[])
    : debug_logs_enabled_(false)
    , current_version_("1.0.0")  // TODO: Load from version file
    , should_exit_(false)
{
    parse_arguments(argc, argv);
    
    if (config_.show_help) {
        print_usage();
        exit(0);
    }
    
    if (config_.show_version) {
        print_version();
        exit(0);
    }
    
    // Set up signal handlers
    g_tray_app_instance = this;
    
#ifdef _WIN32
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
#else
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
#endif
    
    std::cout << "DEBUG: Signal handlers installed" << std::endl;
}

TrayApp::~TrayApp() {
    shutdown();
    g_tray_app_instance = nullptr;
}

int TrayApp::run() {
    std::cout << "DEBUG: TrayApp::run() starting..." << std::endl;
    
    // Find server binary if not specified
    if (config_.server_binary.empty()) {
        std::cout << "DEBUG: Searching for server binary..." << std::endl;
        if (!find_server_binary()) {
            std::cerr << "Error: Could not find lemonade server binary" << std::endl;
            std::cerr << "Please specify --server-binary path" << std::endl;
            return 1;
        }
    }
    
    std::cout << "DEBUG: Using server binary: " << config_.server_binary << std::endl;
    
    // Create server manager
    std::cout << "DEBUG: Creating server manager..." << std::endl;
    server_manager_ = std::make_unique<ServerManager>();
    
    // Start server
    std::cout << "DEBUG: Starting server..." << std::endl;
    if (!start_server()) {
        std::cerr << "Error: Failed to start server" << std::endl;
        return 1;
    }
    
    std::cout << "DEBUG: Server started successfully!" << std::endl;
    
    // If no-tray mode, just wait for server to exit
    if (config_.no_tray) {
        std::cout << "Server running in foreground mode (no tray)" << std::endl;
        std::cout << "Press Ctrl+C to stop" << std::endl;
        
        // TODO: Set up signal handlers for Ctrl+C
        while (server_manager_->is_server_running()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        
        return 0;
    }
    
    std::cout << "DEBUG: Creating tray application..." << std::endl;
    // Create tray application
    tray_ = create_tray();
    if (!tray_) {
        std::cerr << "Error: Failed to create tray for this platform" << std::endl;
        return 1;
    }
    
    std::cout << "DEBUG: Tray created successfully" << std::endl;
    
    // Set ready callback
    std::cout << "DEBUG: Setting ready callback..." << std::endl;
    tray_->set_ready_callback([this]() {
        std::cout << "DEBUG: Ready callback triggered!" << std::endl;
        show_notification("Woohoo!", "Lemonade Server is running! Right-click the tray icon to access options.");
    });
    
    // Set menu update callback to refresh state before showing menu
    std::cout << "DEBUG: Setting menu update callback..." << std::endl;
    if (auto* windows_tray = dynamic_cast<WindowsTray*>(tray_.get())) {
        windows_tray->set_menu_update_callback([this]() {
            std::cout << "DEBUG: Refreshing menu state from server..." << std::endl;
            build_menu();
        });
    }
    
    // Find icon path (matching the CMake resources structure)
    std::cout << "DEBUG: Searching for icon..." << std::endl;
    std::string icon_path = "resources/static/favicon.ico";
    std::cout << "DEBUG: Checking icon at: " << fs::absolute(icon_path).string() << std::endl;
    
    if (!fs::exists(icon_path)) {
        // Try relative to executable directory
        fs::path exe_path = fs::path(config_.server_binary).parent_path();
        icon_path = (exe_path / "resources" / "static" / "favicon.ico").string();
        std::cout << "DEBUG: Icon not found, trying: " << icon_path << std::endl;
        
        // If still not found, try without static subdir (fallback)
        if (!fs::exists(icon_path)) {
            icon_path = (exe_path / "resources" / "favicon.ico").string();
            std::cout << "DEBUG: Icon not found, trying fallback: " << icon_path << std::endl;
        }
    }
    
    if (fs::exists(icon_path)) {
        std::cout << "DEBUG: Icon found at: " << icon_path << std::endl;
    } else {
        std::cout << "WARNING: Icon not found at any location, will use default icon" << std::endl;
    }
    
    // Initialize tray
    std::cout << "DEBUG: Initializing tray with icon: " << icon_path << std::endl;
    if (!tray_->initialize("Lemonade Server", icon_path)) {
        std::cerr << "Error: Failed to initialize tray" << std::endl;
        return 1;
    }
    
    std::cout << "DEBUG: Tray initialized successfully" << std::endl;
    
    // Build initial menu
    std::cout << "DEBUG: Building menu..." << std::endl;
    build_menu();
    std::cout << "DEBUG: Menu built successfully" << std::endl;
    
    std::cout << "DEBUG: Menu built, entering event loop..." << std::endl;
    // Run tray event loop
    tray_->run();
    
    std::cout << "DEBUG: Event loop exited" << std::endl;
    return 0;
}

void TrayApp::parse_arguments(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        
        if (arg == "--help" || arg == "-h") {
            config_.show_help = true;
        } else if (arg == "--version" || arg == "-v") {
            config_.show_version = true;
        } else if (arg == "--no-tray") {
            config_.no_tray = true;
        } else if (arg == "--port" && i + 1 < argc) {
            config_.port = std::stoi(argv[++i]);
        } else if (arg == "--ctx-size" && i + 1 < argc) {
            config_.ctx_size = std::stoi(argv[++i]);
        } else if (arg == "--log-file" && i + 1 < argc) {
            config_.log_file = argv[++i];
        } else if (arg == "--log-level" && i + 1 < argc) {
            config_.log_level = argv[++i];
        } else if (arg == "--server-binary" && i + 1 < argc) {
            config_.server_binary = argv[++i];
        }
    }
}

void TrayApp::print_usage() {
    std::cout << "lemonade-server-beta - Lemonade Server Tray Application\n\n";
    std::cout << "Usage: lemonade-server-beta [options]\n\n";
    std::cout << "Options:\n";
    std::cout << "  --port PORT              Server port (default: 8000)\n";
    std::cout << "  --ctx-size SIZE          Context size (default: 4096)\n";
    std::cout << "  --log-file PATH          Log file path\n";
    std::cout << "  --log-level LEVEL        Log level: debug, info, warning, error\n";
    std::cout << "  --server-binary PATH     Path to lemonade server binary\n";
    std::cout << "  --no-tray                Start server without tray (headless mode)\n";
    std::cout << "  --help, -h               Show this help message\n";
    std::cout << "  --version, -v            Show version\n\n";
    std::cout << "Examples:\n";
    std::cout << "  lemonade-server-beta                    # Start with tray\n";
    std::cout << "  lemonade-server-beta --port 8080        # Start on custom port\n";
    std::cout << "  lemonade-server-beta --no-tray          # Start without tray\n";
}

void TrayApp::print_version() {
    std::cout << "lemonade-server-beta version " << current_version_ << std::endl;
}

bool TrayApp::find_server_binary() {
    // Look for lemonade binary in common locations
    std::vector<std::string> search_paths;
    
#ifdef _WIN32
    std::string binary_name = "lemonade-router.exe";
    
    // Get the directory where this executable is located
    char exe_path_buf[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, exe_path_buf, MAX_PATH);
    if (len > 0) {
        fs::path exe_dir = fs::path(exe_path_buf).parent_path();
        // First priority: same directory as this executable
        search_paths.push_back((exe_dir / binary_name).string());
    }
#else
    std::string binary_name = "lemonade";
    
    // On Unix, try to get executable path
    char exe_path_buf[1024];
    ssize_t len = readlink("/proc/self/exe", exe_path_buf, sizeof(exe_path_buf) - 1);
    if (len != -1) {
        exe_path_buf[len] = '\0';
        fs::path exe_dir = fs::path(exe_path_buf).parent_path();
        search_paths.push_back((exe_dir / binary_name).string());
    }
#endif
    
    // Current directory
    search_paths.push_back(binary_name);
    
    // Parent directory
    search_paths.push_back("../" + binary_name);
    
    // Common install locations
#ifdef _WIN32
    search_paths.push_back("C:/Program Files/Lemonade/" + binary_name);
#else
    search_paths.push_back("/usr/local/bin/" + binary_name);
    search_paths.push_back("/usr/bin/" + binary_name);
#endif
    
    for (const auto& path : search_paths) {
        if (fs::exists(path)) {
            config_.server_binary = fs::absolute(path).string();
            std::cout << "Found server binary: " << config_.server_binary << std::endl;
            return true;
        }
    }
    
    return false;
}

bool TrayApp::setup_logging() {
    // TODO: Implement logging setup
    return true;
}

bool TrayApp::start_server() {
    // Set default log file if not specified
    if (config_.log_file.empty()) {
        #ifdef _WIN32
        // Windows: %TEMP%\lemonade-server.log
        char* temp_path = nullptr;
        size_t len = 0;
        _dupenv_s(&temp_path, &len, "TEMP");
        if (temp_path) {
            config_.log_file = std::string(temp_path) + "\\lemonade-server.log";
            free(temp_path);
        } else {
            config_.log_file = "lemonade-server.log";
        }
        #else
        // Unix: /tmp/lemonade-server.log or ~/.lemonade/server.log
        config_.log_file = "/tmp/lemonade-server.log";
        #endif
        std::cout << "Using default log file: " << config_.log_file << std::endl;
    }
    
    return server_manager_->start_server(
        config_.server_binary,
        config_.port,
        config_.ctx_size,
        config_.log_file
    );
}

void TrayApp::stop_server() {
    if (server_manager_) {
        server_manager_->stop_server();
    }
}

void TrayApp::build_menu() {
    if (!tray_) return;
    
    Menu menu = create_menu();
    tray_->set_menu(menu);
}

Menu TrayApp::create_menu() {
    Menu menu;
    
    // Status display
    std::string loaded = get_loaded_model();
    if (!loaded.empty()) {
        menu.add_item(MenuItem::Action("Loaded: " + loaded, nullptr, false));
        menu.add_item(MenuItem::Action("Unload LLM", [this]() { on_unload_model(); }));
    } else {
        menu.add_item(MenuItem::Action("No models loaded", nullptr, false));
    }
    
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
            bool is_loaded = (model.id == loaded);
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
            [this, size]() { on_change_context_size(size); },
            is_current
        ));
    }
    menu.add_item(MenuItem::Submenu("Context Size", ctx_submenu));
    
    menu.add_separator();
    
    // Main menu items
    menu.add_item(MenuItem::Action("Documentation", [this]() { on_open_documentation(); }));
    menu.add_item(MenuItem::Action("LLM Chat", [this]() { on_open_llm_chat(); }));
    menu.add_item(MenuItem::Action("Model Manager", [this]() { on_open_model_manager(); }));
    
    // Logs submenu
    auto logs_submenu = std::make_shared<Menu>();
    logs_submenu->add_item(MenuItem::Action("Show Logs", [this]() { on_show_logs(); }));
    logs_submenu->add_separator();
    logs_submenu->add_item(MenuItem::Checkable(
        "Enable Debug Logs",
        [this]() { on_toggle_debug_logs(); },
        debug_logs_enabled_
    ));
    menu.add_item(MenuItem::Submenu("Logs", logs_submenu));
    
    menu.add_separator();
    menu.add_item(MenuItem::Action("Quit Lemonade", [this]() { on_quit(); }));
    
    return menu;
}

// Menu action implementations

void TrayApp::on_load_model(const std::string& model_name) {
    std::cout << "Loading model: " << model_name << std::endl;
    if (server_manager_->load_model(model_name)) {
        loaded_model_ = model_name;
        build_menu();
    }
}

void TrayApp::on_unload_model() {
    std::cout << "Unloading model" << std::endl;
    if (server_manager_->unload_model()) {
        loaded_model_.clear();
        build_menu();
    }
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

void TrayApp::on_toggle_debug_logs() {
    debug_logs_enabled_ = !debug_logs_enabled_;
    LogLevel level = debug_logs_enabled_ ? LogLevel::DEBUG : LogLevel::INFO;
    server_manager_->set_log_level(level);
    build_menu();
    show_notification("Debug Logs", debug_logs_enabled_ ? "Debug logs enabled" : "Debug logs disabled");
}

void TrayApp::on_show_logs() {
    if (config_.log_file.empty()) {
        show_notification("Error", "No log file configured");
        return;
    }
    
#ifdef _WIN32
    // Open new PowerShell window with tail-like command
    // Use Start-Process to open a new window that stays open
    std::string cmd = "powershell -Command \"Start-Process powershell -ArgumentList '-NoExit','-Command',\\\"Get-Content -Wait '" + config_.log_file + "'\\\"\"";
    system(cmd.c_str());
#elif defined(__APPLE__)
    // Open Terminal.app with tail command
    std::string cmd = "osascript -e 'tell application \"Terminal\" to do script \"tail -f " + config_.log_file + "\"'";
    system(cmd.c_str());
#else
    // Linux: try gnome-terminal or xterm
    std::string cmd = "gnome-terminal -- tail -f '" + config_.log_file + "' || xterm -e tail -f '" + config_.log_file + "'";
    system(cmd.c_str());
#endif
}

void TrayApp::on_open_documentation() {
    open_url("https://lemonade-server.ai/docs/");
}

void TrayApp::on_open_llm_chat() {
    open_url("http://localhost:" + std::to_string(config_.port) + "/#llm-chat");
}

void TrayApp::on_open_model_manager() {
    open_url("http://localhost:" + std::to_string(config_.port) + "/#model-management");
}

void TrayApp::on_upgrade() {
    // TODO: Implement upgrade functionality
    std::cout << "Upgrade functionality not yet implemented" << std::endl;
}

void TrayApp::on_quit() {
    std::cout << "Quitting application..." << std::endl;
    shutdown();
}

void TrayApp::shutdown() {
    if (should_exit_) {
        return;  // Already shutting down
    }
    
    should_exit_ = true;
    
    std::cout << "Shutting down gracefully..." << std::endl;
    
    // Stop the server
    stop_server();
    
    // Stop the tray
    if (tray_) {
        tray_->stop();
    }
}

void TrayApp::open_url(const std::string& url) {
#ifdef _WIN32
    ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    system(("open \"" + url + "\"").c_str());
#else
    system(("xdg-open \"" + url + "\" &").c_str());
#endif
}

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

std::vector<ModelInfo> TrayApp::get_downloaded_models() {
    try {
        auto models_json = server_manager_->get_models();
        std::vector<ModelInfo> models;
        
        // Parse the models JSON response
        // Expected format: {"data": [{"id": "...", "checkpoint": "...", "recipe": "..."}], "object": "list"}
        if (models_json.contains("data") && models_json["data"].is_array()) {
            std::cout << "DEBUG: Found " << models_json["data"].size() << " models from server" << std::endl;
            
            for (const auto& model : models_json["data"]) {
                ModelInfo info;
                info.id = model.value("id", "");
                info.checkpoint = model.value("checkpoint", "");
                info.recipe = model.value("recipe", "");
                
                if (!info.id.empty()) {
                    std::cout << "DEBUG: Added model: " << info.id << std::endl;
                    models.push_back(info);
                }
            }
        } else {
            std::cout << "DEBUG: No 'data' array in models response" << std::endl;
        }
        
        return models;
    } catch (const std::exception& e) {
        std::cerr << "Failed to get models: " << e.what() << std::endl;
        return {};
    }
}

} // namespace lemon_tray

