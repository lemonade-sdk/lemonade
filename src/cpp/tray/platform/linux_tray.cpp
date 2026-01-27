#if defined(__linux__) && !defined(__ANDROID__)

#include "lemon_tray/platform/linux_tray.h"
#include <iostream>

// Headless stub implementation for Linux
// This avoids LGPL dependencies (GTK3, libappindicator3, libnotify)
// Users should run with --no-tray flag on Linux

namespace lemon_tray {

LinuxTray::LinuxTray()
    : log_level_("info")
    , should_exit_(false)
{
    // Headless mode - no initialization needed
}

LinuxTray::~LinuxTray() {
    // Headless mode - no cleanup needed
}

bool LinuxTray::initialize(const std::string& app_name, const std::string& icon_path) {
    app_name_ = app_name;
    icon_path_ = icon_path;

    if (log_level_ == "debug") {
        std::cout << "[Linux Tray] Headless mode - tray not supported on Linux" << std::endl;
        std::cout << "[Linux Tray] Please use --no-tray flag to run in headless mode" << std::endl;
    }

    // Call ready callback immediately since there's no UI to initialize
    if (ready_callback_) {
        ready_callback_();
    }

    // Return false to indicate tray is not available
    // This will cause the app to fall back to --no-tray behavior
    return false;
}

void LinuxTray::run() {
    if (log_level_ == "debug") {
        std::cout << "[Linux Tray] Headless mode - no event loop to run" << std::endl;
    }
    // No-op in headless mode
}

void LinuxTray::stop() {
    if (log_level_ == "debug") {
        std::cout << "[Linux Tray] Headless mode - stopping" << std::endl;
    }
    should_exit_ = true;
}

void LinuxTray::set_menu(const Menu& menu) {
    if (log_level_ == "debug") {
        std::cout << "[Linux Tray] Headless mode - ignoring menu with "
                  << menu.items.size() << " items" << std::endl;
    }
    // No-op in headless mode
}

void LinuxTray::update_menu() {
    if (log_level_ == "debug") {
        std::cout << "[Linux Tray] Headless mode - ignoring menu update" << std::endl;
    }
    // No-op in headless mode
}

void LinuxTray::show_notification(
    const std::string& title,
    const std::string& message,
    NotificationType /*type*/)
{
    // Print to console instead of showing a GUI notification
    std::cout << "[Notification] " << title << ": " << message << std::endl;
}

void LinuxTray::set_icon(const std::string& icon_path) {
    icon_path_ = icon_path;
    if (log_level_ == "debug") {
        std::cout << "[Linux Tray] Headless mode - ignoring icon: " << icon_path << std::endl;
    }
    // No-op in headless mode
}

void LinuxTray::set_tooltip(const std::string& tooltip) {
    if (log_level_ == "debug") {
        std::cout << "[Linux Tray] Headless mode - ignoring tooltip: " << tooltip << std::endl;
    }
    // No-op in headless mode
}

void LinuxTray::set_ready_callback(std::function<void()> callback) {
    ready_callback_ = callback;
}

void LinuxTray::set_log_level(const std::string& log_level) {
    log_level_ = log_level;
}

} // namespace lemon_tray

#endif // __linux__ && !__ANDROID__
