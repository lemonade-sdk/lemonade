#pragma once

#if defined(__linux__) && !defined(__ANDROID__)

#include "tray_interface.h"

namespace lemon_tray {

class LinuxTray : public TrayInterface {
public:
    LinuxTray();
    ~LinuxTray() override;
    
    // TrayInterface implementation
    bool initialize(const std::string& app_name, const std::string& icon_path) override;
    void run() override;
    void stop() override;
    void set_menu(const Menu& menu) override;
    void update_menu() override;
    void show_notification(
        const std::string& title,
        const std::string& message,
        NotificationType type = NotificationType::INFO
    ) override;
    void set_icon(const std::string& icon_path) override;
    void set_tooltip(const std::string& tooltip) override;
    void set_ready_callback(std::function<void()> callback) override;
    void set_log_level(const std::string& log_level) override;
    
private:
    // Headless implementation - no GUI dependencies
    // Linux tray support disabled to avoid LGPL dependencies
    std::string app_name_;
    std::string icon_path_;
    std::string log_level_;
    std::function<void()> ready_callback_;
    bool should_exit_;
};

} // namespace lemon_tray

#endif // __linux__ && !__ANDROID__

