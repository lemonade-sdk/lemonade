#pragma once

// Linux systemd integration helpers.
// On Linux: types and function declarations (implementations in linux_tray.cpp).
// On non-Linux platforms (macOS, Windows): inline stubs that return safe defaults.

#include <string>

namespace lemon_tray {

#if defined(__linux__) && !defined(__ANDROID__)

enum class SystemdBusScope { System, User };

struct SystemdUnitInfo {
    std::string name;
    SystemdBusScope scope = SystemdBusScope::System;
    explicit operator bool() const { return !name.empty(); }
};

bool is_service_active();
bool is_any_systemd_service_active();
int get_systemd_any_service_main_pid();

#ifdef HAVE_SYSTEMD
bool is_systemd_any_service_active_other_process();
SystemdUnitInfo get_active_systemd_unit_info();
SystemdUnitInfo get_first_known_systemd_unit();
bool systemd_control_service(const SystemdUnitInfo& unit, bool start);
#else
bool systemd_control_service(const char* unit_name, bool start);
#endif // HAVE_SYSTEMD

#else // non-Linux (macOS, Windows, etc.) — safe no-op stubs

inline bool is_service_active() { return false; }
inline bool is_any_systemd_service_active() { return false; }
inline int get_systemd_any_service_main_pid() { return 0; }

#endif // __linux__ && !__ANDROID__

} // namespace lemon_tray
