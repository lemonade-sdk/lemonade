#include "lemon_tray/tray_app.h"
#include <iostream>
#include <exception>

#ifdef _WIN32
#include <windows.h>
#endif

// Console entry point
// This is the CLI client - perfect for terminal use
int main(int argc, char* argv[]) {
    try {
        lemon_tray::TrayApp app(argc, argv);
        return app.run();
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "Unknown fatal error" << std::endl;
        return 1;
    }
}

