#pragma once

#if defined(__linux__) && !defined(__ANDROID__)

#include "tray_interface.h"

#ifdef HAVE_APPINDICATOR
#ifdef HAVE_AYATANA_APPINDICATOR_GLIB
#include <libayatana-appindicator-glib/ayatana-appindicator.h>  // glib variant (GTK-free, GIO only)
#ifdef HAVE_DBUSMENU_GLIB
#include <libdbusmenu-glib/server.h>
#include <libdbusmenu-glib/menuitem.h>
#endif // HAVE_DBUSMENU_GLIB
#else
#include <gtk/gtk.h>
#ifdef HAVE_AYATANA_APPINDICATOR
#include <libayatana-appindicator/app-indicator.h>   // GTK3 Ayatana variant
#else
#include <libappindicator/app-indicator.h>           // upstream fallback
#endif // HAVE_AYATANA_APPINDICATOR
#endif // HAVE_AYATANA_APPINDICATOR_GLIB
#ifdef HAVE_LIBNOTIFY
#include <libnotify/notify.h>
#endif // HAVE_LIBNOTIFY
#endif // HAVE_APPINDICATOR

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

private:
#ifdef HAVE_APPINDICATOR
    AppIndicator* indicator_;
#ifdef HAVE_AYATANA_APPINDICATOR_GLIB
    GMainLoop* main_loop_;
    GMenu* g_menu_;
    GSimpleActionGroup* action_group_;
    std::vector<std::function<void()>*> callbacks_;
    void build_g_menu(const Menu& menu, GMenu* parent, GSimpleActionGroup* actions,
                      int& action_id, std::vector<std::function<void()>*>& callbacks);
#ifdef HAVE_DBUSMENU_GLIB
    DbusmenuServer* dbusmenu_server_;
    void build_dbusmenu(const Menu& menu, DbusmenuMenuitem* parent,
                        std::vector<std::function<void()>*>& callbacks);
#endif // HAVE_DBUSMENU_GLIB
#else
    GtkWidget* gtk_menu_;
    void build_gtk_menu(const Menu& menu, GtkWidget* parent_menu);
#endif // HAVE_AYATANA_APPINDICATOR_GLIB
#endif // HAVE_APPINDICATOR

    std::string app_name_;
    std::string icon_path_;
    std::function<void()> ready_callback_;
    bool should_exit_;
};

} // namespace lemon_tray

#endif // __linux__ && !__ANDROID__
