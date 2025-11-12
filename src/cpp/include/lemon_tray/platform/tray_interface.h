#pragma once

#include <string>
#include <vector>
#include <functional>
#include <memory>

namespace lemon_tray {

// Forward declarations
struct MenuItem;
struct Menu;

// Notification types
enum class NotificationType {
    INFO,
    WARNING,
    ERROR,
    SUCCESS
};

// Menu callback signature
using MenuCallback = std::function<void()>;

// Menu item structure
struct MenuItem {
    std::string text;
    MenuCallback callback;
    bool enabled = true;
    bool checked = false;
    bool is_separator = false;
    std::shared_ptr<Menu> submenu = nullptr;
    int id = -1; // Platform-specific menu item ID
    
    // Factory methods
    static MenuItem Separator() {
        MenuItem item;
        item.is_separator = true;
        item.text = "";
        return item;
    }
    
    static MenuItem Action(const std::string& text, MenuCallback callback, bool enabled = true) {
        MenuItem item;
        item.text = text;
        item.callback = callback;
        item.enabled = enabled;
        return item;
    }
    
    static MenuItem Checkable(const std::string& text, MenuCallback callback, bool checked, bool enabled = true) {
        MenuItem item;
        item.text = text;
        item.callback = callback;
        item.checked = checked;
        item.enabled = enabled;
        return item;
    }
    
    static MenuItem Submenu(const std::string& text, std::shared_ptr<Menu> submenu) {
        MenuItem item;
        item.text = text;
        item.submenu = submenu;
        return item;
    }
};

// Menu structure
struct Menu {
    std::vector<MenuItem> items;
    
    void add_item(const MenuItem& item) {
        items.push_back(item);
    }
    
    void add_separator() {
        items.push_back(MenuItem::Separator());
    }
};

// Abstract tray interface
class TrayInterface {
public:
    virtual ~TrayInterface() = default;
    
    // Lifecycle
    virtual bool initialize(const std::string& app_name, const std::string& icon_path) = 0;
    virtual void run() = 0;
    virtual void stop() = 0;
    
    // Menu management
    virtual void set_menu(const Menu& menu) = 0;
    virtual void update_menu() = 0;
    
    // Notifications
    virtual void show_notification(
        const std::string& title,
        const std::string& message,
        NotificationType type = NotificationType::INFO
    ) = 0;
    
    // Icon and tooltip management
    virtual void set_icon(const std::string& icon_path) = 0;
    virtual void set_tooltip(const std::string& tooltip) = 0;
    
    // Set callback for when ready
    virtual void set_ready_callback(std::function<void()> callback) = 0;
    
    // Set log level for debug logging
    virtual void set_log_level(const std::string& log_level) = 0;
};

// Factory function to create platform-specific tray
std::unique_ptr<TrayInterface> create_tray();

} // namespace lemon_tray

