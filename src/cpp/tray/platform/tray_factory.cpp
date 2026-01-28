#include "lemon_tray/platform/tray_interface.h"

#ifdef _WIN32
#include "lemon_tray/platform/windows_tray.h"
#elif defined(__APPLE__)
#include "lemon_tray/platform/macos_tray.h"
#elif defined(__linux__) && !defined(__ANDROID__)
#include "lemon_tray/platform/linux_tray.h"
#endif

namespace lemon_tray {

std::unique_ptr<TrayInterface> create_tray() {
#ifdef _WIN32
    return std::make_unique<WindowsTray>();
#elif defined(__APPLE__)
    return std::make_unique<MacOSTray>();
#elif defined(__linux__) && !defined(__ANDROID__)
    return std::make_unique<LinuxTray>();
#else
    #error "Unsupported platform"
    return nullptr;
#endif
}

} // namespace lemon_tray
