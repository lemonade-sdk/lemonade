// Simple GUI launcher for Lemonade Server tray application
// This is a minimal WIN32 GUI app that just launches lemonade-server.exe serve

#include <windows.h>
#include <string>
#include <filesystem>
#include <lemon/single_instance.h>

namespace fs = std::filesystem;

// Find lemonade-server.exe (should be in same directory)
std::wstring find_server_exe() {
    // Get directory of this executable
    wchar_t exe_path[MAX_PATH];
    GetModuleFileNameW(NULL, exe_path, MAX_PATH);

    fs::path exe_dir = fs::path(exe_path).parent_path();
    fs::path server_path = exe_dir / L"lemonade-server.exe";

    if (fs::exists(server_path)) {
        return server_path.wstring();
    }

    return L"";
}

// Launch lemonade-server.exe serve
bool launch_server() {
    std::wstring server_exe = find_server_exe();

    if (server_exe.empty()) {
        MessageBoxW(NULL,
            L"Could not find lemonade-server.exe\n\n"
            L"Please ensure lemonade-server.exe is in the same directory as this application.",
            L"Lemonade Server - Error",
            MB_OK | MB_ICONERROR);
        return false;
    }

    // Build command line: lemonade-server.exe serve
    std::wstring cmdline = L"\"" + server_exe + L"\" serve";

    // Launch as a new process
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    // Create the process with hidden console window
    if (!CreateProcessW(
        NULL,                       // Application name (use command line)
        &cmdline[0],               // Command line (modifiable)
        NULL,                       // Process security attributes
        NULL,                       // Thread security attributes
        FALSE,                      // Don't inherit handles
        CREATE_NO_WINDOW,           // Hide console window
        NULL,                       // Environment
        NULL,                       // Current directory
        &si,                        // Startup info
        &pi))                       // Process info
    {
        DWORD error = GetLastError();
        std::wstring error_msg = L"Failed to start Lemonade Server.\n\nError code: " + std::to_wstring(error);
        MessageBoxW(NULL, error_msg.c_str(), L"Lemonade Server - Error", MB_OK | MB_ICONERROR);
        return false;
    }

    // Close our handles (the process continues running)
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return true;
}

// Windows GUI entry point
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow) {
    // Check for single instance
    if (lemon::SingleInstance::IsAnotherInstanceRunning("Tray")) {
        // Try to activate the existing tray instance
        // Note: The actual tray window is created by lemonade-server.exe, not this launcher
        lemon::SingleInstance::ActivateExistingInstance("Lemonade Server");

        MessageBoxW(NULL,
            L"Lemonade Server is already running.\n\n"
            L"Check your system tray for the lemon icon.",
            L"Lemonade Server",
            MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    // Simply launch lemonade-server.exe serve and exit
    if (launch_server()) {
        return 0;  // Success
    } else {
        return 1;  // Error (already showed message box)
    }
}
