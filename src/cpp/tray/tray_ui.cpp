#include "lemon_tray/tray_ui.h"
#include <lemon/utils/path_utils.h>
#include <lemon/version.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <thread>

#ifdef _WIN32
#include "lemon_tray/platform/windows_tray.h"
#include <shellapi.h>
#include <winsock2.h>
#include <windows.h>
#else
#include <cstdlib>
#include <fcntl.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>
#endif


namespace fs = std::filesystem;

namespace lemon_tray {

#ifndef _WIN32
int TrayUI::signal_pipe_[2] = {-1, -1};
#endif

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string TrayUI::get_connect_host() const {
    if (host_.empty() || host_ == "0.0.0.0" || host_ == "localhost") {
        return "127.0.0.1";
    }
    return host_;
}

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

TrayUI::TrayUI(int port, const std::string& host, bool silent)
    : port_(port)
    , host_(host)
    , silent_(silent)
    , recipe_options_(nlohmann::json::object())
{
#ifndef _WIN32
    if (pipe(signal_pipe_) == -1) {
        std::cerr << "Failed to create signal pipe" << std::endl;
    } else {
        // Set write end to non-blocking
        int flags = fcntl(signal_pipe_[1], F_GETFL);
        if (flags != -1) {
            fcntl(signal_pipe_[1], F_SETFL, flags | O_NONBLOCK);
        }
    }
#endif
}

TrayUI::~TrayUI() {
#ifndef _WIN32
    if (signal_monitor_thread_.joinable()) {
        stop_signal_monitor_ = true;
        signal_monitor_thread_.join();
    }
    if (signal_pipe_[0] != -1) {
        close(signal_pipe_[0]);
        close(signal_pipe_[1]);
        signal_pipe_[0] = signal_pipe_[1] = -1;
    }
#endif

}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool TrayUI::initialize() {
    tray_ = create_tray();
    if (!tray_) {
        std::cerr << "Error: Failed to create tray for this platform" << std::endl;
        return false;
    }

    tray_->set_ready_callback([this]() {
        if (!silent_) {
            show_notification("Woohoo!", "Lemonade Server is running! Right-click the tray icon to access options.");
        }
    });

#ifdef _WIN32
    if (auto* windows_tray = dynamic_cast<WindowsTray*>(tray_.get())) {
        windows_tray->set_menu_update_callback([this]() {
            refresh_menu();
        });
    }
#endif

    std::string icon_path = find_icon_path();

    if (!tray_->initialize("Lemonade Server", icon_path)) {
        std::cerr << "Error: Failed to initialize tray" << std::endl;
        return false;
    }

    build_menu();
    return true;
}

void TrayUI::run() {
#ifndef _WIN32
    // Background thread to monitor signals and periodically refresh the menu
    signal_monitor_thread_ = std::thread([this]() {
        auto last_tick = std::chrono::steady_clock::now();
        while (!stop_signal_monitor_) {
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(signal_pipe_[0], &readfds);

            struct timeval tv = {0, 100000};  // 100ms
            int result = select(signal_pipe_[0] + 1, &readfds, nullptr, nullptr, &tv);

            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - last_tick).count() >= 5) {
                refresh_menu();
                last_tick = now;
            }

            if (result > 0 && FD_ISSET(signal_pipe_[0], &readfds)) {
                char sig;
                ssize_t bytes_read = read(signal_pipe_[0], &sig, 1);
                (void)bytes_read;
                std::cout << "\nReceived interrupt signal, shutting down..." << std::endl;
                stop();
                break;
            }
        }
    });
#endif

    tray_->run();  // Blocks in platform event loop
}

void TrayUI::stop() {
    if (tray_) {
        tray_->stop();
    }
}

// ---------------------------------------------------------------------------
// HTTP helpers
// ---------------------------------------------------------------------------

std::string TrayUI::http_get(const std::string& endpoint) {
    httplib::Client cli(get_connect_host(), port_);
    cli.set_connection_timeout(2);
    cli.set_read_timeout(5);

    // Pass API key if set
    const char* api_key = std::getenv("LEMONADE_API_KEY");
    httplib::Headers headers;
    if (api_key && api_key[0]) {
        headers.emplace("Authorization", std::string("Bearer ") + api_key);
    }

    auto res = cli.Get(endpoint, headers);
    if (res && res->status == 200) {
        return res->body;
    }
    return "";
}

std::string TrayUI::http_post(const std::string& endpoint, const std::string& body) {
    httplib::Client cli(get_connect_host(), port_);
    cli.set_connection_timeout(2);
    cli.set_read_timeout(30);

    const char* api_key = std::getenv("LEMONADE_API_KEY");
    httplib::Headers headers;
    if (api_key && api_key[0]) {
        headers.emplace("Authorization", std::string("Bearer ") + api_key);
    }

    auto res = cli.Post(endpoint, headers, body, "application/json");
    if (res && (res->status == 200 || res->status == 204)) {
        return res->body;
    }
    return "";
}

// ---------------------------------------------------------------------------
// Data fetchers
// ---------------------------------------------------------------------------

std::pair<bool, std::vector<LoadedModelInfo>> TrayUI::fetch_server_state() {
    std::vector<LoadedModelInfo> loaded_models;
    try {
        std::string body = http_get("/api/v1/health");
        if (body.empty()) return {false, loaded_models};

        auto health = nlohmann::json::parse(body);
        if (health.contains("all_models_loaded") && health["all_models_loaded"].is_array()) {
            for (const auto& model : health["all_models_loaded"]) {
                LoadedModelInfo info;
                info.model_name = model.value("model_name", "");
                info.checkpoint = model.value("checkpoint", "");
                info.last_use = model.value("last_use", 0.0);
                info.type = model.value("type", "llm");
                info.device = model.value("device", "");
                info.backend_url = model.value("backend_url", "");
                if (!info.model_name.empty()) {
                    loaded_models.push_back(info);
                }
            }
        }
        return {true, loaded_models};
    } catch (...) {
        return {false, loaded_models};
    }
}

std::vector<LoadedModelInfo> TrayUI::get_all_loaded_models() {
    return fetch_server_state().second;
}

std::vector<ModelInfo> TrayUI::get_downloaded_models() {
    try {
        std::string body = http_get("/api/v1/models");
        if (body.empty()) return {};

        auto models_json = nlohmann::json::parse(body);
        std::vector<ModelInfo> models;

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
        }
        return models;
    } catch (...) {
        return {};
    }
}

// ---------------------------------------------------------------------------
// Menu building
// ---------------------------------------------------------------------------

void TrayUI::build_menu() {
    if (!tray_) return;

    // Fetch once, use for both the menu and the cache
    auto [reachable, loaded_models] = fetch_server_state();
    auto available_models = get_downloaded_models();

    Menu menu = create_menu(loaded_models, available_models);
    tray_->set_menu(menu);

    std::lock_guard<std::mutex> lock(state_mutex_);
    last_menu_server_reachable_ = reachable;
    last_menu_loaded_models_ = std::move(loaded_models);
    last_menu_available_models_ = std::move(available_models);
}

void TrayUI::refresh_menu() {
    if (!tray_) return;
    if (menu_needs_refresh()) {
        build_menu();
    }
}

bool TrayUI::menu_needs_refresh() {
    // Fetch outside the lock to avoid blocking other threads during HTTP calls
    auto [reachable, loaded] = fetch_server_state();
    auto current_available = get_downloaded_models();

    std::lock_guard<std::mutex> lock(state_mutex_);
    if (reachable != last_menu_server_reachable_) return true;
    if (loaded != last_menu_loaded_models_) return true;
    if (current_available != last_menu_available_models_) return true;
    return false;
}

Menu TrayUI::create_menu(const std::vector<LoadedModelInfo>& loaded_models,
                         const std::vector<ModelInfo>& available_models) {
    Menu menu;

    // Open app — uses lemonade:// protocol, falls back to web app
    menu.add_item(MenuItem::Action("Open Lemonade App", [this]() { open_desktop_app(); }));
    menu.add_separator();

    // Use pre-fetched data (passed from build_menu to avoid redundant HTTP calls)
    std::set<std::string> loaded_model_names;
    for (const auto& m : loaded_models) {
        loaded_model_names.insert(m.model_name);
    }

    if (is_loading_model_) {
        std::lock_guard<std::mutex> lock(loading_mutex_);
        menu.add_item(MenuItem::Action("Loading: " + loading_model_name_ + "...", nullptr, false));
    } else if (!loaded_models.empty()) {
        for (const auto& model : loaded_models) {
            std::string text = "Loaded: " + model.model_name;
            if (!model.type.empty() && model.type != "llm") {
                text += " (" + model.type + ")";
            }
            menu.add_item(MenuItem::Action(text, nullptr, false));
        }
    } else {
        menu.add_item(MenuItem::Action("No models loaded", nullptr, false));
    }

    // Unload submenu
    auto unload_submenu = std::make_shared<Menu>();
    if (loaded_models.empty()) {
        unload_submenu->add_item(MenuItem::Action("No models loaded", nullptr, false));
    } else {
        for (const auto& model : loaded_models) {
            std::string text = model.model_name;
            if (!model.type.empty() && model.type != "llm") {
                text += " (" + model.type + ")";
            }
            unload_submenu->add_item(MenuItem::Action(
                text,
                [this, name = model.model_name]() { on_unload_specific_model(name); }
            ));
        }
        if (loaded_models.size() > 1) {
            unload_submenu->add_separator();
            unload_submenu->add_item(MenuItem::Action("Unload all", [this]() { on_unload_model(); }));
        }
    }
    menu.add_item(MenuItem::Submenu("Unload Model", unload_submenu));

    // Load submenu
    auto load_submenu = std::make_shared<Menu>();
    if (available_models.empty()) {
        load_submenu->add_item(MenuItem::Action("No models available: Use the Model Manager", nullptr, false));
    } else {
        for (const auto& model : available_models) {
            bool is_loaded = loaded_model_names.count(model.id) > 0;
            load_submenu->add_item(MenuItem::Checkable(
                model.id,
                [this, id = model.id]() { on_load_model(id); },
                is_loaded
            ));
        }
    }
    menu.add_item(MenuItem::Submenu("Load Model", load_submenu));

    // Port submenu
    auto port_submenu = std::make_shared<Menu>();
    std::vector<std::pair<int, std::string>> ports = {
        {13305, "Port 13305"}, {8000, "Port 8000"}, {8020, "Port 8020"}, {8040, "Port 8040"},
        {8060, "Port 8060"}, {8080, "Port 8080"}, {9000, "Port 9000"},
        {11434, "Port 11434 (Ollama)"},
    };
    for (const auto& [port, label] : ports) {
        port_submenu->add_item(MenuItem::Checkable(
            label,
            [this, p = port]() { on_change_port(p); },
            port == port_
        ));
    }
    menu.add_item(MenuItem::Submenu("Port", port_submenu));

    // Context Size submenu
    auto ctx_submenu = std::make_shared<Menu>();
    std::vector<std::pair<std::string, int>> ctx_sizes = {
        {"4K", 4096}, {"8K", 8192}, {"16K", 16384},
        {"32K", 32768}, {"64K", 65536}, {"128K", 131072},
        {"256K", 262144}
    };
    for (const auto& [label, size] : ctx_sizes) {
        bool is_current = recipe_options_.contains("ctx_size") &&
                          (size == recipe_options_["ctx_size"]);
        ctx_submenu->add_item(MenuItem::Checkable(
            "Context size " + label,
            [this, s = size]() { on_change_context_size(s); },
            is_current
        ));
    }
    menu.add_item(MenuItem::Submenu("Context Size", ctx_submenu));

    menu.add_separator();
    menu.add_item(MenuItem::Action("Documentation", [this]() { on_open_documentation(); }));
    menu.add_item(MenuItem::Action("Show Logs", [this]() { on_show_logs(); }));

    menu.add_separator();
    menu.add_item(MenuItem::Action("Quit Lemonade", [this]() { on_quit(); }));

    return menu;
}

// ---------------------------------------------------------------------------
// Menu actions
// ---------------------------------------------------------------------------

void TrayUI::on_load_model(const std::string& model_name) {
    std::string name_copy = model_name;

    if (is_loading_model_) {
        show_notification("Model Loading", "A model is already being loaded. Please wait.");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(loading_mutex_);
        is_loading_model_ = true;
        loading_model_name_ = name_copy;
    }
    build_menu();

    std::thread([this, name_copy]() {
        nlohmann::json body;
        body["model_name"] = name_copy;
        std::string result = http_post("/api/v1/load", body.dump());

        {
            std::lock_guard<std::mutex> lock(loading_mutex_);
            is_loading_model_ = false;
        }
        build_menu();

        if (!result.empty()) {
            show_notification("Model Loaded", "Successfully loaded " + name_copy);
        } else {
            show_notification("Load Failed", "Failed to load " + name_copy);
        }
    }).detach();
}

void TrayUI::on_unload_model() {
    if (is_loading_model_) {
        show_notification("Model Loading", "Please wait for the current model to finish loading.");
        return;
    }
    http_post("/api/v1/unload");
    build_menu();
}

void TrayUI::on_unload_specific_model(const std::string& model_name) {
    std::string name_copy = model_name;

    if (is_loading_model_) {
        show_notification("Model Loading", "Please wait for the current model to finish loading.");
        return;
    }

    std::thread([this, name_copy]() {
        nlohmann::json body;
        body["model_name"] = name_copy;
        http_post("/api/v1/unload", body.dump());
        build_menu();
    }).detach();
}

void TrayUI::on_change_port(int new_port) {
    nlohmann::json body;
    body["port"] = new_port;
    std::string result = http_post("/internal/set", body.dump());
    if (!result.empty()) {
        port_ = new_port;
        build_menu();
        show_notification("Port Changed", "Lemonade Server is now running on port " + std::to_string(new_port));
    }
}

void TrayUI::on_change_context_size(int new_ctx_size) {
    recipe_options_["ctx_size"] = new_ctx_size;
    nlohmann::json body;
    body["ctx_size"] = new_ctx_size;
    http_post("/api/v1/params", body.dump());
    build_menu();

    std::string label = (new_ctx_size >= 1024)
        ? std::to_string(new_ctx_size / 1024) + "K"
        : std::to_string(new_ctx_size);
    show_notification("Context Size Changed", "Lemonade Server context size is now " + label);
}

void TrayUI::on_show_logs() {
    open_desktop_app("view=logs");
}

void TrayUI::on_open_documentation() {
    open_url("https://lemonade-server.ai/docs/");
}

void TrayUI::on_quit() {
    std::cout << "Quitting application..." << std::endl;
    stop();
}

// ---------------------------------------------------------------------------
// App launch helpers
// ---------------------------------------------------------------------------

// Open a URL via the OS without invoking a shell (avoids shell injection).
// On macOS/Linux, we fork+execlp the opener directly.
#ifndef _WIN32
static int exec_open_url_tray(const char* opener, const std::string& url, bool wait) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        // Child: redirect stdout/stderr to /dev/null
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDOUT_FILENO); dup2(devnull, STDERR_FILENO); close(devnull); }
        execlp(opener, opener, url.c_str(), nullptr);
        _exit(127);  // execlp failed
    }
    if (wait) {
        int status = 0;
        waitpid(pid, &status, 0);
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    return 0;  // fire-and-forget
}
#endif

void TrayUI::open_url(const std::string& url) {
#ifdef _WIN32
    ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    exec_open_url_tray("open", url, false);
#else
    exec_open_url_tray("xdg-open", url, false);
#endif
}

bool TrayUI::try_open_lemonade_url(const std::string& lemonade_url) {
    // Ask the OS to open the lemonade:// URL.
    // Returns true if the OS reports success (handler registered).
#ifdef _WIN32
    // Check registry before calling ShellExecuteA — Windows shows a "Get an app"
    // dialog for unregistered URI schemes and still returns > 32 (success).
    HKEY hKey = nullptr;
    if (RegOpenKeyExA(HKEY_CLASSES_ROOT, "lemonade", 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return false;
    }
    RegCloseKey(hKey);
    HINSTANCE result = ShellExecuteA(nullptr, "open", lemonade_url.c_str(),
                                     nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<intptr_t>(result) > 32;
#elif defined(__APPLE__)
    return exec_open_url_tray("open", lemonade_url, true) == 0;
#else
    return exec_open_url_tray("xdg-open", lemonade_url, true) == 0;
#endif
}

void TrayUI::open_desktop_app(const std::string& route) {
    // Construct lemonade:// URL
    std::string url = "lemonade://open";
    if (!route.empty()) {
        url += "?" + route;
    }

    // Try lemonade:// protocol first; fall back to web app
    if (!try_open_lemonade_url(url)) {
        open_web_app(route);
    }
}

void TrayUI::open_web_app(const std::string& route) {
    std::string url = "http://" + get_connect_host() + ":" + std::to_string(port_) + "/";
    if (!route.empty()) {
        url += "?" + route;
    }
    open_url(url);
}

// ---------------------------------------------------------------------------
// Icon discovery
// ---------------------------------------------------------------------------

std::string TrayUI::find_icon_path() {
    fs::path exe_dir = lemon::utils::get_executable_dir();

#ifdef __APPLE__
    std::string path = "/Library/Application Support/lemonade/resources/static/favicon.ico";
    if (fs::exists(path)) return path;
#elif defined(__linux__)
    // Search XDG data directories
    std::vector<std::string> data_dirs;
    const char* xdg_data_home = getenv("XDG_DATA_HOME");
    if (xdg_data_home && xdg_data_home[0]) {
        data_dirs.push_back(xdg_data_home);
    } else {
        const char* home = getenv("HOME");
        if (home && home[0]) data_dirs.push_back(std::string(home) + "/.local/share");
    }
    const char* xdg_data_dirs = getenv("XDG_DATA_DIRS");
    if (xdg_data_dirs && xdg_data_dirs[0]) {
        std::istringstream ss(xdg_data_dirs);
        std::string d;
        while (std::getline(ss, d, ':')) {
            if (!d.empty()) data_dirs.push_back(d);
        }
    } else {
        data_dirs.push_back("/usr/local/share");
        data_dirs.push_back("/usr/share");
        data_dirs.push_back("/opt/lemonade/share");
    }
    for (const auto& d : data_dirs) {
        auto svg = fs::path(d) / "icons/hicolor/scalable/apps/ai.lemonade_server.Lemonade.svg";
        if (fs::exists(svg)) return svg.string();
        auto ico = fs::path(d) / "lemonade-server/resources/static/favicon.ico";
        if (fs::exists(ico)) return ico.string();
    }
#endif

    // Common fallbacks: relative to exe directory
    if (!exe_dir.empty()) {
        std::vector<fs::path> paths = {
            exe_dir / "resources" / "static" / "favicon.ico",
            exe_dir / "resources" / "favicon.ico",
        };
        for (const auto& p : paths) {
            if (fs::exists(p)) return p.string();
        }
    }

    // CWD fallback
    if (fs::exists("resources/static/favicon.ico")) {
        return "resources/static/favicon.ico";
    }

    return "";
}

// ---------------------------------------------------------------------------
// Notifications
// ---------------------------------------------------------------------------

void TrayUI::show_notification(const std::string& title, const std::string& message) {
    if (tray_) {
        tray_->show_notification(title, message);
    }
}

} // namespace lemon_tray
