// Electron app integration for lemonade-server CLI
// These are TrayApp methods extracted to a separate file for organization.

#include "lemon_server/tray_app.h"
#include <iostream>
#include <filesystem>
#include <thread>
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <unistd.h>
#include <signal.h>
#include <cstring>
#include <climits>
#endif

namespace fs = std::filesystem;

#if defined(__linux__)
#include <fstream>
#include <sys/types.h>

// Helper function to check if a process is alive (and not a zombie)
static bool is_process_alive_not_zombie(pid_t pid) {
    if (pid <= 0) return false;
    
    // First check if process exists at all
    if (kill(pid, 0) != 0) {
        return false;  // Process doesn't exist
    }
    
    // Check if it's a zombie by reading /proc/PID/stat
    std::string stat_path = "/proc/" + std::to_string(pid) + "/stat";
    std::ifstream stat_file(stat_path);
    if (!stat_file) {
        return false;  // Can't read stat, assume dead
    }
    
    std::string line;
    std::getline(stat_file, line);
    
    // Find the state character (after the closing paren of the process name)
    size_t paren_pos = line.rfind(')');
    if (paren_pos != std::string::npos && paren_pos + 2 < line.length()) {
        char state = line[paren_pos + 2];
        // Return false if zombie
        return (state != 'Z');
    }
    
    return true;  // Assume alive if we can't determine
}
#endif

namespace lemon_server {

// ============================================================
// Electron App Integration
// ============================================================

bool TrayApp::find_electron_app() {
    // Get directory of this executable (lemonade-tray.exe)
    fs::path exe_dir;
    
#ifdef _WIN32
    wchar_t exe_path[MAX_PATH];
    GetModuleFileNameW(NULL, exe_path, MAX_PATH);
    exe_dir = fs::path(exe_path).parent_path();
#else
    char exe_path[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len != -1) {
        exe_path[len] = '\0';
        exe_dir = fs::path(exe_path).parent_path();
    } else {
        return false;
    }
#endif
    
    // The Electron app has exactly two possible locations:
    // 1. Production (WIX installer): ../app/ relative to bin/ directory
    // 2. Development: same directory (copied by CopyElectronApp.cmake)
    // 3. Linux production: /usr/local/share/lemonade-server/app/lemonade
    
#ifdef _WIN32
    constexpr const char* exe_name = "Lemonade.exe";
#elif defined(__APPLE__)
    constexpr const char* exe_name = "Lemonade.app";
#else
    constexpr const char* exe_name = "lemonade";
#endif
    
#if defined(__linux__)
    // On Linux, check the production installation path first
    // If the executable is in /usr/local/bin, the app is in /usr/local/share/lemonade-server/app/
    if (exe_dir == "/usr/local/bin") {
        fs::path linux_production_path = fs::path("/usr/local/share/lemonade-server/app") / exe_name;
        if (fs::exists(linux_production_path)) {
            electron_app_path_ = fs::canonical(linux_production_path).string();
            std::cout << "Found Electron app at: " << electron_app_path_ << std::endl;
            return true;
        }
    }
#endif
    
    // Check production path first (most common case)
    fs::path production_path = exe_dir / ".." / "app" / exe_name;
    if (fs::exists(production_path)) {
        electron_app_path_ = fs::canonical(production_path).string();
        std::cout << "Found Electron app at: " << electron_app_path_ << std::endl;
        return true;
    }
    
    // Check development path (same directory as tray executable)
    fs::path dev_path = exe_dir / exe_name;
    if (fs::exists(dev_path)) {
        electron_app_path_ = fs::canonical(dev_path).string();
        std::cout << "Found Electron app at: " << electron_app_path_ << std::endl;
        return true;
    }
    
    std::cerr << "Warning: Could not find Electron app" << std::endl;
    std::cerr << "  Checked: " << production_path.string() << std::endl;
    std::cerr << "  Checked: " << dev_path.string() << std::endl;
    return false;
}

void TrayApp::launch_electron_app() {
    // Try to find the app if we haven't already
    if (electron_app_path_.empty()) {
        if (!find_electron_app()) {
            std::cerr << "Error: Cannot launch Electron app - not found" << std::endl;
            return;
        }
    }
    
#ifdef _WIN32
    // Single-instance enforcement: Only allow one Electron app to be open at a time
    // Reuse child process tracking to determine if the app is already running
    if (electron_app_process_ != nullptr) {
        // Check if the process is still alive
        DWORD exit_code = 0;
        if (GetExitCodeProcess(electron_app_process_, &exit_code) && exit_code == STILL_ACTIVE) {
            std::cout << "Electron app is already running" << std::endl;
            show_notification("App Already Running", "The Lemonade app is already open");
            return;
        } else {
            // Process has exited, clean up the handle
            CloseHandle(electron_app_process_);
            electron_app_process_ = nullptr;
        }
    }
#endif
    
    // Launch the Electron app
#ifdef _WIN32
    // Windows: Create a job object to ensure the Electron app closes when tray closes
    if (!electron_job_object_) {
        electron_job_object_ = CreateJobObjectA(NULL, NULL);
        if (electron_job_object_) {
            // Configure job to terminate all processes when the last handle is closed
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_info = {};
            job_info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            
            if (!SetInformationJobObject(
                electron_job_object_,
                JobObjectExtendedLimitInformation,
                &job_info,
                sizeof(job_info))) {
                std::cerr << "Warning: Failed to configure job object: " << GetLastError() << std::endl;
                CloseHandle(electron_job_object_);
                electron_job_object_ = nullptr;
            } else {
                std::cout << "Created job object for Electron app process management" << std::endl;
            }
        } else {
            std::cerr << "Warning: Failed to create job object: " << GetLastError() << std::endl;
        }
    }
    
    // Launch the .exe
    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    
    // Create the process
    if (CreateProcessA(
        electron_app_path_.c_str(),  // Application name
        NULL,                         // Command line
        NULL,                         // Process security attributes
        NULL,                         // Thread security attributes
        FALSE,                        // Don't inherit handles
        CREATE_SUSPENDED,             // Create suspended so we can add to job before it runs
        NULL,                         // Environment
        NULL,                         // Current directory
        &si,                          // Startup info
        &pi))                         // Process info
    {
        // Add the process to the job object if we have one
        if (electron_job_object_) {
            if (AssignProcessToJobObject(electron_job_object_, pi.hProcess)) {
                std::cout << "Added Electron app to job object (will close with tray)" << std::endl;
            } else {
                std::cerr << "Warning: Failed to add process to job object: " << GetLastError() << std::endl;
            }
        }
        
        // Resume the process now that it's in the job object
        ResumeThread(pi.hThread);
        
        // Store the process handle (don't close it - we need it for cleanup)
        electron_app_process_ = pi.hProcess;
        CloseHandle(pi.hThread);  // We don't need the thread handle
        
        std::cout << "Launched Electron app" << std::endl;
    } else {
        std::cerr << "Failed to launch Electron app: " << GetLastError() << std::endl;
    }
#elif defined(__APPLE__)
    // Single-instance enforcement: Check if the Electron app is already running
    if (electron_app_pid_ > 0) {
        // Check if the process is still alive
        if (kill(electron_app_pid_, 0) == 0) {
            std::cout << "Electron app is already running (PID: " << electron_app_pid_ << ")" << std::endl;
            show_notification("App Already Running", "The Lemonade app is already open");
            return;
        } else {
            // Process has exited, reset the PID
            electron_app_pid_ = 0;
        }
    }
    
    // macOS: Use 'open' command to launch the .app
    // Note: 'open' doesn't give us the PID directly, so we'll need to find it
    std::string cmd = "open \"" + electron_app_path_ + "\"";
    int result = system(cmd.c_str());
    if (result == 0) {
        std::cout << "Launched Electron app" << std::endl;
        
        // Try to find the PID of the Electron app we just launched
        // Look for process named "Lemonade" (the app name, not the .app bundle name)
        std::this_thread::sleep_for(std::chrono::milliseconds(500));  // Give it time to start
        FILE* pipe = popen("pgrep -n Lemonade", "r");  // -n = newest matching process
        if (pipe) {
            char buffer[128];
            if (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
                electron_app_pid_ = atoi(buffer);
                std::cout << "Tracking Electron app (PID: " << electron_app_pid_ << ")" << std::endl;
            }
            pclose(pipe);
        }
    } else {
        std::cerr << "Failed to launch Electron app" << std::endl;
    }
#else
    // Single-instance enforcement: Check if the Electron app is already running
    if (electron_app_pid_ > 0) {
        // Check if the process is still alive (and not a zombie)
        if (is_process_alive_not_zombie(electron_app_pid_)) {
            std::cout << "Electron app is already running (PID: " << electron_app_pid_ << ")" << std::endl;
            show_notification("App Already Running", "The Lemonade app is already open");
            return;
        } else {
            // Process has exited, reset the PID
            electron_app_pid_ = 0;
        }
    }
    
    // Linux: Launch the binary directly using fork/exec for proper PID tracking
    pid_t pid = fork();
    if (pid == 0) {
        // Child process: execute the Electron app
        execl(electron_app_path_.c_str(), electron_app_path_.c_str(), nullptr);
        // If execl returns, it failed
        std::cerr << "Failed to execute Electron app: " << strerror(errno) << std::endl;
        _exit(1);
    } else if (pid > 0) {
        // Parent process: store the PID
        electron_app_pid_ = pid;
        std::cout << "Launched Electron app (PID: " << electron_app_pid_ << ")" << std::endl;
    } else {
        // Fork failed
        std::cerr << "Failed to launch Electron app: " << strerror(errno) << std::endl;
    }
#endif
}

// ============================================================
// URL Opening Utility
// ============================================================

void TrayApp::open_url(const std::string& url) {
#ifdef _WIN32
    ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    int result = system(("open \"" + url + "\"").c_str());
    (void)result;  // Suppress unused variable warning
#else
    int result = system(("xdg-open \"" + url + "\" &").c_str());
    (void)result;  // Suppress unused variable warning
#endif
}

} // namespace lemon_server

