#if defined(__linux__) && !defined(__ANDROID__)

#include "lemon_tray/platform/linux_tray.h"
#include <iostream>

// TODO: Include GTK and libappindicator headers
// #include <gtk/gtk.h>
// #include <libappindicator/app-indicator.h>
// #include <libnotify/notify.h>

namespace lemon_tray {

LinuxTray::LinuxTray()
    : indicator_(nullptr)
    , gtk_menu_(nullptr)
    , should_exit_(false)
{
    // TODO: Initialize GTK
    // gtk_init(nullptr, nullptr);
}

LinuxTray::~LinuxTray() {
    // TODO: Clean up GTK and AppIndicator objects
}

bool LinuxTray::initialize(const std::string& app_name, const std::string& icon_path) {
    app_name_ = app_name;
    icon_path_ = icon_path;
    
    std::cout << "[Linux Tray] TODO: Initialize system tray" << std::endl;
    std::cout << "[Linux Tray] App name: " << app_name << std::endl;
    std::cout << "[Linux Tray] Icon path: " << icon_path << std::endl;
    
    // TODO: Implement using libappindicator3
    /*
    indicator_ = app_indicator_new(
        "lemonade-server",
        icon_path.c_str(),
        APP_INDICATOR_CATEGORY_APPLICATION_STATUS
    );
    app_indicator_set_status(indicator_, APP_INDICATOR_STATUS_ACTIVE);
    app_indicator_set_title(indicator_, app_name.c_str());
    */
    
    if (ready_callback_) {
        ready_callback_();
    }
    
    return false; // Not implemented yet
}

void LinuxTray::run() {
    std::cout << "[Linux Tray] TODO: Run GTK main loop" << std::endl;
    // TODO: gtk_main();
}

void LinuxTray::stop() {
    std::cout << "[Linux Tray] TODO: Stop GTK main loop" << std::endl;
    should_exit_ = true;
    // TODO: gtk_main_quit();
}

void LinuxTray::set_menu(const Menu& menu) {
    std::cout << "[Linux Tray] TODO: Set menu with " << menu.items.size() << " items" << std::endl;
    
    // TODO: Build GtkMenu from Menu structure
    /*
    GtkWidget *gtk_menu = gtk_menu_new();
    
    for (const auto& item : menu.items) {
        GtkWidget *menu_item;
        if (item.is_separator) {
            menu_item = gtk_separator_menu_item_new();
        } else {
            menu_item = gtk_menu_item_new_with_label(item.text.c_str());
            // Connect callback
        }
        gtk_menu_shell_append(GTK_MENU_SHELL(gtk_menu), menu_item);
        gtk_widget_show(menu_item);
    }
    
    app_indicator_set_menu(indicator_, GTK_MENU(gtk_menu));
    */
}

void LinuxTray::update_menu() {
    std::cout << "[Linux Tray] TODO: Update menu" << std::endl;
    // TODO: Rebuild menu
}

void LinuxTray::show_notification(
    const std::string& title,
    const std::string& message,
    NotificationType type)
{
    std::cout << "[Linux Tray] TODO: Show notification: " << title << " - " << message << std::endl;
    
    // TODO: Use libnotify
    /*
    notify_init("Lemonade Server");
    NotifyNotification *n = notify_notification_new(
        title.c_str(),
        message.c_str(),
        nullptr  // icon
    );
    notify_notification_show(n, nullptr);
    g_object_unref(G_OBJECT(n));
    notify_uninit();
    */
}

void LinuxTray::set_icon(const std::string& icon_path) {
    icon_path_ = icon_path;
    std::cout << "[Linux Tray] TODO: Set icon: " << icon_path << std::endl;
    // TODO: app_indicator_set_icon_full(indicator_, icon_path.c_str(), "Lemonade");
}

void LinuxTray::set_tooltip(const std::string& tooltip) {
    std::cout << "[Linux Tray] TODO: Set tooltip: " << tooltip << std::endl;
    // TODO: app_indicator_set_title(indicator_, tooltip.c_str());
}

void LinuxTray::set_ready_callback(std::function<void()> callback) {
    ready_callback_ = callback;
}

} // namespace lemon_tray

#endif // __linux__ && !__ANDROID__

