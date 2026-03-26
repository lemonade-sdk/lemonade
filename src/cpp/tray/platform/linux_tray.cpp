#if defined(__linux__) && !defined(__ANDROID__)

#include "lemon_tray/platform/linux_tray.h"
#include "lemon_tray/platform/linux_systemd.h"
#include <lemon/utils/aixlog.hpp>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <optional>
#include <utility>
#include <unistd.h>

#ifdef HAVE_SYSTEMD
#include <systemd/sd-bus.h>
#include <systemd/sd-daemon.h>
#include <systemd/sd-login.h>
#endif // HAVE_SYSTEMD

namespace fs = std::filesystem;

#ifdef HAVE_DBUSMENU_GLIB
// dbusmenu-glib 16.04.0 predates GLib autoptr support; define cleanup here
// so g_autoptr(DbusmenuMenuitem) is visible throughout this translation unit.
G_DEFINE_AUTOPTR_CLEANUP_FUNC(DbusmenuMenuitem, g_object_unref)
#endif // HAVE_DBUSMENU_GLIB

namespace lemon_tray {

// ── systemd integration helpers ───────────────────────────────────────────────────
// Detect and interact with systemd-managed lemonade server instances.

// Known systemd unit names for lemonade server (native or snap)
static const char* kSystemdUnitNames[] = {
    "lemonade-server.service",
    "snap.lemonade-server.daemon.service"
};

// Shared search directories for systemd unit files (system paths only; user path is appended dynamically).
static constexpr const char* kSystemdSystemDirs[] = {
    "/etc/systemd/system/",
    "/usr/lib/systemd/system/",
    "/lib/systemd/system/",
};

#ifdef HAVE_SYSTEMD

// Internal: check if a unit is active on a specific D-Bus (system or user session)
static bool is_systemd_service_active_on_bus(const char* unit_name, SystemdBusScope scope) {
    if (!unit_name || unit_name[0] == '\0') {
        return false;
    }

    if (sd_booted() <= 0) {
        return false;
    }

    sd_bus* bus = nullptr;
    sd_bus_message* reply = nullptr;
    sd_bus_error error = SD_BUS_ERROR_NULL;

    int r = (scope == SystemdBusScope::User)
        ? sd_bus_open_user(&bus)
        : sd_bus_open_system(&bus);
    if (r < 0 || !bus) {
        sd_bus_error_free(&error);
        return false;
    }

    r = sd_bus_call_method(
        bus,
        "org.freedesktop.systemd1",
        "/org/freedesktop/systemd1",
        "org.freedesktop.systemd1.Manager",
        "GetUnit",
        &error,
        &reply,
        "s",
        unit_name
    );

    if (r < 0 || !reply) {
        sd_bus_error_free(&error);
        sd_bus_unref(bus);
        return false;
    }

    const char* unit_path = nullptr;
    r = sd_bus_message_read(reply, "o", &unit_path);
    sd_bus_message_unref(reply);
    reply = nullptr;

    if (r < 0 || !unit_path) {
        sd_bus_error_free(&error);
        sd_bus_unref(bus);
        return false;
    }

    char* active_state = nullptr;
    r = sd_bus_get_property_string(
        bus,
        "org.freedesktop.systemd1",
        unit_path,
        "org.freedesktop.systemd1.Unit",
        "ActiveState",
        &error,
        &active_state
    );

    if (r < 0 || !active_state) {
        sd_bus_error_free(&error);
        sd_bus_unref(bus);
        return false;
    }

    bool is_active =
        (strcmp(active_state, "active") == 0) ||
        (strcmp(active_state, "activating") == 0) ||
        (strcmp(active_state, "reloading") == 0);

    free(active_state);
    sd_bus_error_free(&error);
    sd_bus_unref(bus);

    return is_active;
}

// Internal: get MainPID of a unit on a specific D-Bus (returns 0 if unavailable)
static int get_systemd_service_main_pid_on_bus(const char* unit_name, SystemdBusScope scope) {
    if (!unit_name || unit_name[0] == '\0') {
        return 0;
    }

    if (sd_booted() <= 0) {
        return 0;
    }

    sd_bus* bus = nullptr;
    sd_bus_message* reply = nullptr;
    sd_bus_error error = SD_BUS_ERROR_NULL;

    int r = (scope == SystemdBusScope::User)
        ? sd_bus_open_user(&bus)
        : sd_bus_open_system(&bus);
    if (r < 0 || !bus) {
        sd_bus_error_free(&error);
        return 0;
    }

    r = sd_bus_call_method(
        bus,
        "org.freedesktop.systemd1",
        "/org/freedesktop/systemd1",
        "org.freedesktop.systemd1.Manager",
        "GetUnit",
        &error,
        &reply,
        "s",
        unit_name
    );

    if (r < 0 || !reply) {
        sd_bus_error_free(&error);
        sd_bus_unref(bus);
        return 0;
    }

    const char* unit_path = nullptr;
    r = sd_bus_message_read(reply, "o", &unit_path);
    sd_bus_message_unref(reply);
    reply = nullptr;

    if (r < 0 || !unit_path) {
        sd_bus_error_free(&error);
        sd_bus_unref(bus);
        return 0;
    }

    unsigned int main_pid = 0;
    r = sd_bus_get_property_trivial(
        bus,
        "org.freedesktop.systemd1",
        unit_path,
        "org.freedesktop.systemd1.Service",
        "MainPID",
        &error,
        'u',
        &main_pid
    );

    if (r < 0) {
        sd_bus_error_free(&error);
        sd_bus_unref(bus);
        return 0;
    }

    sd_bus_error_free(&error);
    sd_bus_unref(bus);

    return static_cast<int>(main_pid);
}

// Returns active unit info (system bus has priority over user bus)
SystemdUnitInfo get_active_systemd_unit_info() {
    for (const auto* unit_name : kSystemdUnitNames) {
        if (is_systemd_service_active_on_bus(unit_name, SystemdBusScope::System))
            return {unit_name, SystemdBusScope::System};
        if (is_systemd_service_active_on_bus(unit_name, SystemdBusScope::User))
            return {unit_name, SystemdBusScope::User};
    }
    return {};
}

// Returns which bus scope owns a unit file, or nullopt if no unit file found
static std::optional<SystemdBusScope> get_unit_file_scope(const char* unit_name) {
    if (!unit_name || unit_name[0] == '\0') return std::nullopt;
    for (const auto* d : kSystemdSystemDirs) {
        if (fs::exists(std::string(d) + unit_name)) return SystemdBusScope::System;
    }
    const char* home = getenv("HOME");
    if (home && fs::exists(std::string(home) + "/.config/systemd/user/" + unit_name)) {
        return SystemdBusScope::User;
    }
    return std::nullopt;
}

// Returns the first known systemd unit that is active or has an installed unit file
SystemdUnitInfo get_first_known_systemd_unit() {
    for (const auto* unit : kSystemdUnitNames) {
        // Active check takes priority over file-based scope detection
        for (auto scope : {SystemdBusScope::System, SystemdBusScope::User}) {
            if (is_systemd_service_active_on_bus(unit, scope))
                return {unit, scope};
        }
        auto file_scope = get_unit_file_scope(unit);
        if (file_scope) return {unit, *file_scope};
    }
    return {};
}

// Start or stop a systemd service.
bool systemd_control_service(const SystemdUnitInfo& unit, bool start) {
    if (!unit) return false;
    std::string action = start ? "start" : "stop";
    if (unit.scope == SystemdBusScope::User) {
        std::string cmd = "systemctl --user " + action + " " + unit.name + " 2>/dev/null";
        return system(cmd.c_str()) == 0;
    }
    // System service: try direct, fall back to pkexec
    std::string cmd = "systemctl " + action + " " + unit.name + " 2>/dev/null";
    if (system(cmd.c_str()) == 0) return true;
    cmd = "pkexec systemctl " + action + " " + unit.name;
    return system(cmd.c_str()) == 0;
}

#endif // HAVE_SYSTEMD

// Check if systemd is running and a unit is active (system or user bus)
static bool is_systemd_service_active(const char* unit_name) {
#ifdef HAVE_SYSTEMD
    return is_systemd_service_active_on_bus(unit_name, SystemdBusScope::System) ||
           is_systemd_service_active_on_bus(unit_name, SystemdBusScope::User);
#else
    (void)unit_name;
    return false;
#endif // HAVE_SYSTEMD
}

// Get systemd service MainPID (system bus first, then user bus)
static int get_systemd_service_main_pid(const char* unit_name) {
#ifdef HAVE_SYSTEMD
    int pid = get_systemd_service_main_pid_on_bus(unit_name, SystemdBusScope::System);
    if (pid > 0) return pid;
    return get_systemd_service_main_pid_on_bus(unit_name, SystemdBusScope::User);
#else
    (void)unit_name;
    return 0;
#endif // HAVE_SYSTEMD
}

// Check if systemd service is active in another process (not this one)
static bool is_systemd_service_active_other_process(const char* unit_name) {
#ifdef HAVE_SYSTEMD
    if (!is_systemd_service_active(unit_name)) {
        return false;
    }

    int main_pid = get_systemd_service_main_pid(unit_name);
    if (main_pid <= 0) {
        return false;
    }

    return (main_pid != getpid());
#else
    (void)unit_name;
    return false;
#endif // HAVE_SYSTEMD
}

bool is_any_systemd_service_active() {
#ifdef HAVE_SYSTEMD
    return static_cast<bool>(get_active_systemd_unit_info());
#else
    return false;
#endif // HAVE_SYSTEMD
}

#ifndef HAVE_SYSTEMD
bool systemd_control_service(const char* unit_name, bool start) {
    if (!unit_name) return false;
    std::string action = start ? "start" : "stop";
    std::string cmd = "systemctl " + action + " " + std::string(unit_name) + " 2>/dev/null";
    if (system(cmd.c_str()) == 0) return true;
    // Fall back to pkexec (graphical polkit dialog for system services)
    cmd = "pkexec systemctl " + action + " " + std::string(unit_name);
    return system(cmd.c_str()) == 0;
}
#endif // !HAVE_SYSTEMD

int get_systemd_any_service_main_pid() {
#ifdef HAVE_SYSTEMD
    auto unit = get_active_systemd_unit_info();
    if (!unit) return 0;
    return get_systemd_service_main_pid_on_bus(unit.name.c_str(), unit.scope);
#else
    return 0;
#endif // HAVE_SYSTEMD
}

bool is_systemd_any_service_active_other_process() {
    for (const auto* unit_name : kSystemdUnitNames) {
        if (is_systemd_service_active_other_process(unit_name)) {
            return true;
        }
    }
    return false;
}

bool is_service_active() {
#ifdef HAVE_SYSTEMD
    return is_any_systemd_service_active();
#else
    return false;
#endif // HAVE_SYSTEMD
}

// ─────────────────────────────────────────────────────────────────────────────────

#ifdef HAVE_APPINDICATOR

// ── GLib-variant static helpers ───────────────────────────────────────────────────

#ifdef HAVE_AYATANA_APPINDICATOR_GLIB

static void on_glib_action_activate(GSimpleAction* /*action*/, GVariant* /*value*/, gpointer data) {
    auto* cb = static_cast<std::function<void()>*>(data);
    if (cb && *cb) (*cb)();
}

static void on_glib_check_activate(GSimpleAction* action, GVariant* /*value*/, gpointer data) {
    GVariant* state = g_action_get_state(G_ACTION(action));
    gboolean active = g_variant_get_boolean(state);
    g_variant_unref(state);
    g_action_change_state(G_ACTION(action), g_variant_new_boolean(!active));
    auto* cb = static_cast<std::function<void()>*>(data);
    if (cb && *cb) (*cb)();
}

#else // GTK3-variant static helper

static void on_menu_item_activate(GtkWidget* /*widget*/, gpointer data) {
    auto* item = static_cast<MenuItem*>(data);
    if (item && item->callback) {
        item->callback();
    }
}

#endif // HAVE_AYATANA_APPINDICATOR_GLIB

// ── Constructor / Destructor ──────────────────────────────────────────────────────

LinuxTray::LinuxTray()
    : indicator_(nullptr)
#ifdef HAVE_AYATANA_APPINDICATOR_GLIB
    , main_loop_(nullptr)
    , g_menu_(nullptr)
    , action_group_(nullptr)
#ifdef HAVE_DBUSMENU_GLIB
    , dbusmenu_server_(nullptr)
#endif // HAVE_DBUSMENU_GLIB
#else
    , gtk_menu_(nullptr)
#endif // HAVE_AYATANA_APPINDICATOR_GLIB
    , should_exit_(false)
{
}

LinuxTray::~LinuxTray() {
    if (indicator_) {
        g_object_unref(G_OBJECT(indicator_));
    }
#ifdef HAVE_AYATANA_APPINDICATOR_GLIB
    if (main_loop_) {
        g_main_loop_unref(main_loop_);
    }
    if (g_menu_) {
        g_object_unref(g_menu_);
    }
    if (action_group_) {
        g_object_unref(action_group_);
    }
    for (auto* cb : callbacks_) {
        delete cb;
    }
#ifdef HAVE_DBUSMENU_GLIB
    if (dbusmenu_server_) {
        g_object_unref(dbusmenu_server_);
    }
#endif // HAVE_DBUSMENU_GLIB
#endif // HAVE_AYATANA_APPINDICATOR_GLIB
}

// ── initialize ────────────────────────────────────────────────────────────────────

bool LinuxTray::initialize(const std::string& app_name, const std::string& icon_path) {
    app_name_ = app_name;
    icon_path_ = icon_path;

#ifdef HAVE_AYATANA_APPINDICATOR_GLIB
    main_loop_ = g_main_loop_new(nullptr, FALSE);
#else
    // The GTK3 Ayatana variant emits a runtime deprecation warning pointing to the glib variant.
    // Suppress it since there is nothing actionable for users seeing our log output.
    g_log_set_handler("libayatana-appindicator", G_LOG_LEVEL_WARNING,
        [](const gchar*, GLogLevelFlags, const gchar*, gpointer) {}, nullptr);

    if (!gtk_init_check(0, nullptr)) {
        std::cerr << "Failed to initialize GTK" << std::endl;
        return false;
    }
#endif // HAVE_AYATANA_APPINDICATOR_GLIB

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    indicator_ = app_indicator_new(
        app_name.c_str(),
        "indicator-messages",
        APP_INDICATOR_CATEGORY_APPLICATION_STATUS
    );
#pragma GCC diagnostic pop

    if (!indicator_) {
        std::cerr << "Failed to create AppIndicator" << std::endl;
        return false;
    }

#ifdef HAVE_AYATANA_APPINDICATOR_GLIB
    // Register an empty GMenuModel and DBusMenu before the indicator becomes visible.
    // GNOME Shell queries com.canonical.dbusmenu immediately on discovery; both
    // interfaces must be present before APP_INDICATOR_STATUS_ACTIVE is set.
    g_menu_ = g_menu_new();
    action_group_ = g_simple_action_group_new();
    app_indicator_set_menu(indicator_, g_menu_);
    app_indicator_set_actions(indicator_, action_group_);
#ifdef HAVE_DBUSMENU_GLIB
    {
        std::string clean = app_name;
        for (auto& c : clean)
            if (!g_ascii_isalnum(c)) c = '_';
        dbusmenu_server_ = dbusmenu_server_new(("/org/ayatana/appindicator/" + clean).c_str());
        g_autoptr(DbusmenuMenuitem) root = dbusmenu_menuitem_new();
        dbusmenu_server_set_root(dbusmenu_server_, root);
    }
#endif // HAVE_DBUSMENU_GLIB
#endif // HAVE_AYATANA_APPINDICATOR_GLIB

    app_indicator_set_status(indicator_, APP_INDICATOR_STATUS_ACTIVE);
    set_icon(icon_path);

#ifdef HAVE_LIBNOTIFY
    notify_init(app_name.c_str());
#endif // HAVE_LIBNOTIFY

    return true;
}

// ── run ───────────────────────────────────────────────────────────────────────────

void LinuxTray::run() {
    if (ready_callback_) {
        g_idle_add([](gpointer data) -> gboolean {
            static_cast<LinuxTray*>(data)->ready_callback_();
            return G_SOURCE_REMOVE;
        }, this);
    }

#ifdef HAVE_AYATANA_APPINDICATOR_GLIB
    g_main_loop_run(main_loop_);
#else
    gtk_main();
#endif // HAVE_AYATANA_APPINDICATOR_GLIB

#ifdef HAVE_LIBNOTIFY
    if (notify_is_initted()) {
        notify_uninit();
    }
#endif // HAVE_LIBNOTIFY
}

// ── stop ──────────────────────────────────────────────────────────────────────────

void LinuxTray::stop() {
#ifdef HAVE_AYATANA_APPINDICATOR_GLIB
    GMainLoop* loop = main_loop_;
    g_idle_add([](gpointer data) -> gboolean {
        g_main_loop_quit(static_cast<GMainLoop*>(data));
        return G_SOURCE_REMOVE;
    }, loop);
#else
    g_idle_add([](gpointer) -> gboolean {
        gtk_main_quit();
        return G_SOURCE_REMOVE;
    }, nullptr);
#endif // HAVE_AYATANA_APPINDICATOR_GLIB
}

// ── set_menu ─────────────────────────────────────────────────────────────────────

void LinuxTray::set_menu(const Menu& menu) {
    auto* menu_copy = new Menu(menu);

#ifdef HAVE_AYATANA_APPINDICATOR_GLIB
    g_idle_add([](gpointer data) -> gboolean {
        auto* params = static_cast<std::pair<LinuxTray*, Menu*>*>(data);
        LinuxTray* self = params->first;
        Menu* menu = params->second;

        // Build new resources first, then swap — keeps the indicator consistent.
        GMenu* new_menu = g_menu_new();
        GSimpleActionGroup* new_group = g_simple_action_group_new();
        std::vector<std::function<void()>*> new_callbacks;
        int action_id = 0;
        self->build_g_menu(*menu, new_menu, new_group, action_id, new_callbacks);

        app_indicator_set_menu(self->indicator_, new_menu);
        app_indicator_set_actions(self->indicator_, new_group);

#ifdef HAVE_DBUSMENU_GLIB
        if (self->dbusmenu_server_) {
            g_autoptr(DbusmenuMenuitem) root = dbusmenu_menuitem_new();
            self->build_dbusmenu(*menu, root, new_callbacks);
            dbusmenu_server_set_root(self->dbusmenu_server_, root);
        }
#endif // HAVE_DBUSMENU_GLIB

        // Release old resources now that the indicator holds refs to the new ones.
        if (self->g_menu_)       g_object_unref(self->g_menu_);
        if (self->action_group_) g_object_unref(self->action_group_);
        for (auto* cb : self->callbacks_) delete cb;

        self->g_menu_       = new_menu;
        self->action_group_ = new_group;
        self->callbacks_    = std::move(new_callbacks);

        delete menu;
        delete params;
        return G_SOURCE_REMOVE;
    }, new std::pair<LinuxTray*, Menu*>(this, menu_copy));
#else // !HAVE_AYATANA_APPINDICATOR_GLIB
    g_idle_add([](gpointer data) -> gboolean {
        auto* params = static_cast<std::pair<LinuxTray*, Menu*>*>(data);
        LinuxTray* self = params->first;
        Menu* menu = params->second;

        if (self->gtk_menu_) {
            gtk_widget_destroy(self->gtk_menu_);
        }

        self->gtk_menu_ = gtk_menu_new();
        self->build_gtk_menu(*menu, self->gtk_menu_);

        gtk_widget_show_all(self->gtk_menu_);
        app_indicator_set_menu(self->indicator_, GTK_MENU(self->gtk_menu_));

        delete menu;
        delete params;
        return G_SOURCE_REMOVE;
    }, new std::pair<LinuxTray*, Menu*>(this, menu_copy));
#endif // HAVE_AYATANA_APPINDICATOR_GLIB
}

// ── build_g_menu (glib variant) ───────────────────────────────────────────────────

#ifdef HAVE_AYATANA_APPINDICATOR_GLIB

void LinuxTray::build_g_menu(const Menu& menu, GMenu* parent, GSimpleActionGroup* actions,
                              int& action_id, std::vector<std::function<void()>*>& callbacks) {
    GMenu* section = g_menu_new();

    for (const auto& item : menu.items) {
        if (item.is_separator) {
            g_menu_append_section(parent, nullptr, G_MENU_MODEL(section));
            g_object_unref(section);
            section = g_menu_new();
            continue;
        }

        if (item.submenu) {
            g_autoptr(GMenu) submenu = g_menu_new();
            build_g_menu(*item.submenu, submenu, actions, action_id, callbacks);

            // g_menu_item_new_submenu() creates a pure submenu-parent item with no
            // activatable action. Using g_menu_item_new() + g_menu_item_set_submenu()
            // sets both "action" and "submenu" attributes, which causes the DBUSMENU
            // serialiser in ayatana-appindicator-glib to discard the submenu link.
            g_autoptr(GMenuItem) menu_item = g_menu_item_new_submenu(item.text.c_str(),
                                                                     G_MENU_MODEL(submenu));
            g_menu_append_item(section, menu_item);
            continue;
        }

        std::string action_name = "action-" + std::to_string(action_id++);
        std::string full_name   = "indicator." + action_name;

        g_autoptr(GSimpleAction) action = item.is_checkable
            ? g_simple_action_new_stateful(action_name.c_str(), nullptr,
                                           g_variant_new_boolean(item.checked ? TRUE : FALSE))
            : g_simple_action_new(action_name.c_str(), nullptr);
        g_simple_action_set_enabled(action, item.enabled ? TRUE : FALSE);

        if (item.callback) {
            auto* cb = new std::function<void()>(item.callback);
            callbacks.push_back(cb);
            if (item.is_checkable) {
                g_signal_connect(action, "activate", G_CALLBACK(on_glib_check_activate), cb);
            } else {
                g_signal_connect(action, "activate", G_CALLBACK(on_glib_action_activate), cb);
            }
        }

        g_action_map_add_action(G_ACTION_MAP(actions), G_ACTION(action));

        g_autoptr(GMenuItem) menu_item = g_menu_item_new(item.text.c_str(), full_name.c_str());
        g_menu_append_item(section, menu_item);
    }

    if (g_menu_model_get_n_items(G_MENU_MODEL(section)) > 0) {
        g_menu_append_section(parent, nullptr, G_MENU_MODEL(section));
    }
    g_object_unref(section);
}

#endif // HAVE_AYATANA_APPINDICATOR_GLIB

// ── build_dbusmenu (dbusmenu-glib bridge) ─────────────────────────────────────────
//
// ayatana-appindicator-glib exports menus via org.gtk.Menus (GMenuModel), but
// GNOME Shell's AppIndicator extension only speaks com.canonical.dbusmenu.
// DbusmenuServer registers that interface on D-Bus at the same object path as
// the AppIndicator, so GNOME Shell finds it when it introspects the indicator.
//
// Ownership: each DbusmenuMenuitem is appended to its parent (which takes a ref),
// then unreffed here — net ref-count stays at 1, held by the parent.  The root
// item is held by DbusmenuServer; replacing the root via dbusmenu_server_set_root()
// drops the old tree automatically.  Callbacks are heap-allocated and tracked in
// `callbacks`; the caller deletes them when the menu is next replaced.

#ifdef HAVE_DBUSMENU_GLIB

void LinuxTray::build_dbusmenu(const Menu& menu, DbusmenuMenuitem* parent,
                                std::vector<std::function<void()>*>& callbacks) {
    for (const auto& item : menu.items) {
        g_autoptr(DbusmenuMenuitem) mi = dbusmenu_menuitem_new();

        if (item.is_separator) {
            // The DBusMenu spec uses type="separator" for horizontal dividers.
            dbusmenu_menuitem_property_set(mi, DBUSMENU_MENUITEM_PROP_TYPE, "separator");
        } else {
            dbusmenu_menuitem_property_set(mi, DBUSMENU_MENUITEM_PROP_LABEL, item.text.c_str());
            dbusmenu_menuitem_property_set_bool(mi, DBUSMENU_MENUITEM_PROP_ENABLED,
                                               item.enabled ? TRUE : FALSE);
            if (item.is_checkable) {
                // Renders as a checkmark item; TOGGLE_STATE reflects the current checked value.
                dbusmenu_menuitem_property_set(mi, DBUSMENU_MENUITEM_PROP_TOGGLE_TYPE,
                                             DBUSMENU_MENUITEM_TOGGLE_CHECK);
                dbusmenu_menuitem_property_set_int(mi, DBUSMENU_MENUITEM_PROP_TOGGLE_STATE,
                    item.checked ? DBUSMENU_MENUITEM_TOGGLE_STATE_CHECKED
                                 : DBUSMENU_MENUITEM_TOGGLE_STATE_UNCHECKED);
            }
            if (item.callback) {
                // The lambda is stateless so +[] converts it to a plain function pointer
                // suitable for GSignal.  The heap-allocated cb outlives the signal because
                // it is deleted by the caller only after dbusmenu_server_set_root() drops
                // the old item tree (and thereby disconnects all signals on it).
                auto* cb = new std::function<void()>(item.callback);
                callbacks.push_back(cb);
                g_signal_connect(mi, DBUSMENU_MENUITEM_SIGNAL_ITEM_ACTIVATED,
                    G_CALLBACK(+[](DbusmenuMenuitem*, guint, gpointer data) {
                        (*static_cast<std::function<void()>*>(data))();
                    }), cb);
            }
            if (item.submenu) {
                // CHILD_DISPLAY_SUBMENU tells the shell to render an arrow/flyout.
                dbusmenu_menuitem_property_set(mi, DBUSMENU_MENUITEM_PROP_CHILD_DISPLAY,
                                             DBUSMENU_MENUITEM_CHILD_DISPLAY_SUBMENU);
                build_dbusmenu(*item.submenu, mi, callbacks);
            }
        }

        dbusmenu_menuitem_child_append(parent, mi);
    }
}

#endif // HAVE_DBUSMENU_GLIB

// ── build_gtk_menu (GTK3 variant) ─────────────────────────────────────────────────

#ifndef HAVE_AYATANA_APPINDICATOR_GLIB

void LinuxTray::build_gtk_menu(const Menu& menu, GtkWidget* parent_menu) {
    for (const auto& item : menu.items) {
        GtkWidget* gtk_item = nullptr;

        if (item.is_separator) {
            gtk_item = gtk_separator_menu_item_new();
        } else if (item.submenu) {
            gtk_item = gtk_menu_item_new_with_label(item.text.c_str());
            GtkWidget* submenu = gtk_menu_new();
            build_gtk_menu(*item.submenu, submenu);
            gtk_menu_item_set_submenu(GTK_MENU_ITEM(gtk_item), submenu);
        } else if (item.is_checkable) {
            gtk_item = gtk_check_menu_item_new_with_label(item.text.c_str());
            gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(gtk_item), item.checked ? TRUE : FALSE);
        } else {
            gtk_item = gtk_menu_item_new_with_label(item.text.c_str());
        }

        gtk_widget_set_sensitive(gtk_item, item.enabled);

        if (item.callback) {
            auto* persistent_item = new MenuItem(item);
            g_object_set_data_full(G_OBJECT(gtk_item), "menu-item-data", persistent_item,
                [](gpointer data) { delete static_cast<MenuItem*>(data); });
            g_signal_connect(gtk_item, "activate", G_CALLBACK(on_menu_item_activate), persistent_item);
        }

        gtk_menu_shell_append(GTK_MENU_SHELL(parent_menu), gtk_item);
    }
}

#endif // !HAVE_AYATANA_APPINDICATOR_GLIB

// ── update_menu ───────────────────────────────────────────────────────────────────

void LinuxTray::update_menu() {
    // No-op, handled by set_menu
}

// ── show_notification ─────────────────────────────────────────────────────────────

void LinuxTray::show_notification(const std::string& title, const std::string& message, NotificationType type) {
    struct NotifData { std::string title, message; NotificationType type; };
    auto* d = new NotifData{title, message, type};

    g_idle_add([](gpointer data) -> gboolean {
        auto* d = static_cast<NotifData*>(data);

#ifdef HAVE_LIBNOTIFY
        NotifyNotification* n = notify_notification_new(d->title.c_str(), d->message.c_str(), nullptr);
        notify_notification_set_timeout(n, 3000);
        NotifyUrgency urgency = (d->type == NotificationType::ERROR)   ? NOTIFY_URGENCY_CRITICAL :
                                (d->type == NotificationType::WARNING) ? NOTIFY_URGENCY_NORMAL   :
                                                                         NOTIFY_URGENCY_LOW;
        notify_notification_set_urgency(n, urgency);
        notify_notification_show(n, nullptr);
        g_object_unref(G_OBJECT(n));
#else // !HAVE_LIBNOTIFY
        LOG(DEBUG, "Notification") << d->title << ": " << d->message << std::endl;
#endif // HAVE_LIBNOTIFY

        delete d;
        return G_SOURCE_REMOVE;
    }, d);
}

// ── set_icon ──────────────────────────────────────────────────────────────────────

void LinuxTray::set_icon(const std::string& icon_path) {
    auto* params = new std::pair<LinuxTray*, std::string>(this, icon_path);

    g_idle_add([](gpointer data) -> gboolean {
        auto* params = static_cast<std::pair<LinuxTray*, std::string>*>(data);
        LinuxTray* self = params->first;
        fs::path icon{params->second};

        if (self->indicator_) {
            if (icon.is_absolute()) {
                std::string stem = icon.stem().string();
                if (params->second.find("/icons/hicolor/") != std::string::npos) {
                    // Installed in the hicolor theme; AppIndicator/SNI resolves it by name.
#ifdef HAVE_AYATANA_APPINDICATOR_GLIB
                    app_indicator_set_icon(self->indicator_, stem.c_str(), "Lemonade");
#else
                    app_indicator_set_icon_full(self->indicator_, stem.c_str(), "Lemonade");
#endif // HAVE_AYATANA_APPINDICATOR_GLIB
                } else {
                    // Dev build: set the parent directory as an extra icon theme path.
                    // Note: AppIndicator expects XDG icon theme structure under this path,
                    // so a flat .ico file won't resolve; the indicator will fall back to
                    // its default icon.  This is acceptable for development builds.
                    app_indicator_set_icon_theme_path(self->indicator_, icon.parent_path().c_str());
#ifdef HAVE_AYATANA_APPINDICATOR_GLIB
                    app_indicator_set_icon(self->indicator_, stem.c_str(), "Lemonade");
#else
                    app_indicator_set_icon_full(self->indicator_, stem.c_str(), "Lemonade");
#endif // HAVE_AYATANA_APPINDICATOR_GLIB
                }
            } else if (!params->second.empty()) {
                // Bare icon name — resolved via the system icon theme.
#ifdef HAVE_AYATANA_APPINDICATOR_GLIB
                app_indicator_set_icon(self->indicator_, params->second.c_str(), "Lemonade");
#else
                app_indicator_set_icon_full(self->indicator_, params->second.c_str(), "Lemonade");
#endif // HAVE_AYATANA_APPINDICATOR_GLIB
            }
        }

        delete params;
        return G_SOURCE_REMOVE;
    }, params);
}

// ── set_tooltip ───────────────────────────────────────────────────────────────────

void LinuxTray::set_tooltip(const std::string& /*tooltip*/) {
    // AppIndicator does not expose a tooltip API.
}

// ── set_ready_callback ────────────────────────────────────────────────────────────

void LinuxTray::set_ready_callback(std::function<void()> callback) {
    ready_callback_ = callback;
}

#else // !HAVE_APPINDICATOR

// ── Headless implementations ──────────────────────────────────────────────────────

LinuxTray::LinuxTray()
    : should_exit_(false)
{
    // Headless mode - no initialization needed
}

LinuxTray::~LinuxTray() {
    // Headless mode - no cleanup needed
}

bool LinuxTray::initialize(const std::string& app_name, const std::string& icon_path) {
    app_name_ = app_name;
    icon_path_ = icon_path;

    LOG(DEBUG, "LinuxTray") << "Headless mode - tray not supported on Linux" << std::endl;
    LOG(DEBUG, "LinuxTray") << "Please use --no-tray flag to run in headless mode" << std::endl;

    // Call ready callback immediately since there's no UI to initialize
    if (ready_callback_) {
        ready_callback_();
    }

    // Return false to indicate tray is not available
    // This will cause the app to fall back to --no-tray behavior
    return false;
}

void LinuxTray::run() {
    LOG(DEBUG, "LinuxTray") << "Headless mode - no event loop to run" << std::endl;
    // No-op in headless mode
}

void LinuxTray::stop() {
    LOG(DEBUG, "LinuxTray") << "Headless mode - stopping" << std::endl;
    should_exit_ = true;
}

void LinuxTray::set_menu(const Menu& menu) {
    LOG(DEBUG, "LinuxTray") << "Headless mode - ignoring menu with "
        << menu.items.size() << " items" << std::endl;
    // No-op in headless mode
}

void LinuxTray::update_menu() {
    LOG(DEBUG, "LinuxTray") << "Headless mode - ignoring menu update" << std::endl;
    // No-op in headless mode
}

void LinuxTray::show_notification(
    const std::string& title,
    const std::string& message,
    NotificationType /*type*/)
{
    // Print to console instead of showing a GUI notification
    LOG(INFO, "Notification") << title << ": " << message << std::endl;
}

void LinuxTray::set_icon(const std::string& icon_path) {
    icon_path_ = icon_path;
    LOG(DEBUG, "LinuxTray") << "Headless mode - ignoring icon: " << icon_path << std::endl;
    // No-op in headless mode
}

void LinuxTray::set_tooltip(const std::string& tooltip) {
    LOG(DEBUG, "LinuxTray") << "Headless mode - ignoring tooltip: " << tooltip << std::endl;
    // No-op in headless mode
}

void LinuxTray::set_ready_callback(std::function<void()> callback) {
    ready_callback_ = callback;
}

#endif // HAVE_APPINDICATOR

} // namespace lemon_tray

#endif // __linux__ && !__ANDROID__
