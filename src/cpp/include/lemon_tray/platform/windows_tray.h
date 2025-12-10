#pragma once

#ifdef _WIN32

#include "tray_interface.h"
#include <windows.h>
#include <shellapi.h>

// Undefine Windows macros that conflict with our code
#ifdef ERROR
#undef ERROR
#endif

#include <map>
#include <atomic>

namespace lemon_tray {

class WindowsTray : public TrayInterface {
public:
    WindowsTray();
    ~WindowsTray() override;

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

    // Set callback for when menu needs updating (e.g., before showing)
    void set_menu_update_callback(std::function<void()> callback) {
        menu_update_callback_ = callback;
    }

    // Check if debug logging is enabled
    bool is_debug() const { return log_level_ == "debug" || log_level_ == "trace"; }

private:
    // Windows-specific methods
    bool register_window_class();
    bool create_window();
    bool add_tray_icon();
    void remove_tray_icon();
    HMENU create_popup_menu(const Menu& menu);
    void add_menu_items(HMENU hmenu, const std::vector<MenuItem>& items);
    void show_context_menu();

    // Window procedure
    static LRESULT CALLBACK window_proc_static(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
    LRESULT window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

    // Message handlers
    void on_tray_icon(LPARAM lparam);
    void on_command(WPARAM wparam);
    void on_destroy();

    // Member variables
    std::string app_name_;
    std::string icon_path_;
    std::string tooltip_;
    std::string log_level_;
    HWND hwnd_;
    HINSTANCE hinst_;
    NOTIFYICONDATAW nid_;
    HMENU hmenu_;
    HICON notification_icon_;  // Icon used for notifications
    std::string last_notification_title_;  // Track last notification for click handling
    std::atomic<bool> should_exit_;
    std::function<void()> ready_callback_;
    std::function<void()> menu_update_callback_;

    // Menu management
    Menu current_menu_;
    std::map<int, MenuCallback> menu_callbacks_;
    int next_menu_id_;

    // Constants
    static constexpr UINT WM_TRAYICON = WM_USER + 1;
    static constexpr int MENU_ID_START = 1000;
};

} // namespace lemon_tray

#endif // _WIN32
