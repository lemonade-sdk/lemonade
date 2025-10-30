#include "lemon_tray/tray_app.h"
#include <iostream>
#include <exception>

#ifdef _WIN32
#include <windows.h>

// Windows GUI entry point (no console window)
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    // Get command line arguments
    int argc = __argc;
    char** argv = __argv;
    
    try {
        lemon_tray::TrayApp app(argc, argv);
        return app.run();
    } catch (const std::exception& e) {
        MessageBoxA(NULL, e.what(), "Lemonade Server Beta - Fatal Error", MB_OK | MB_ICONERROR);
        return 1;
    } catch (...) {
        MessageBoxA(NULL, "Unknown fatal error", "Lemonade Server Beta - Fatal Error", MB_OK | MB_ICONERROR);
        return 1;
    }
}
#else
// Unix/Console entry point
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
#endif

