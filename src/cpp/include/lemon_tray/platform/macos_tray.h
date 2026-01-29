#pragma once

#ifdef __APPLE__

#include "tray_interface.h"

namespace lemon_tray {

class MacOSTray : public TrayInterface {
public:
    MacOSTray();
    ~MacOSTray() override;

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
    void* impl_; // Pointer to Objective-C implementation
    std::string tooltip_;
    std::string app_name_;
    std::string icon_path_;
    std::string log_level_;
    std::function<void()> ready_callback_;
    bool should_exit_;
};

} // namespace lemon_tray

#endif // __APPLE__
