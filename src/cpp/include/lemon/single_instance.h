#pragma once

#include <string>
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <sys/file.h>
#include <errno.h>
#endif

namespace lemon {

class SingleInstance {
public:
    // Check if another instance is running
    // Returns true if another instance is already running
    static bool IsAnotherInstanceRunning(const std::string& app_name) {
#ifdef _WIN32
        // Use Global namespace for system-wide mutex
        std::string mutex_name = "Global\\Lemonade" + app_name + "Mutex";
        
        // Try to create or open the mutex
        HANDLE mutex = CreateMutexA(NULL, TRUE, mutex_name.c_str());
        DWORD error = GetLastError();
        
        if (error == ERROR_ALREADY_EXISTS) {
            // Another instance is running
            if (mutex) CloseHandle(mutex);
            return true;
        }
        
        // Keep mutex handle alive for lifetime of process
        // Store it so it doesn't get cleaned up
        static HANDLE persistent_mutex = mutex;
        (void)persistent_mutex; // Suppress unused warning
        
        return false;
#else
        // Unix/Linux: Use file locking
        std::string lock_file = "/tmp/lemonade_" + app_name + ".lock";
        int fd = open(lock_file.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0666);
        if (fd == -1) return false;
        
        // Try to acquire exclusive lock (non-blocking)
        if (flock(fd, LOCK_EX | LOCK_NB) == -1) {
            close(fd);
            return errno == EWOULDBLOCK; // Another instance has the lock
        }
        
        // Keep fd open for lifetime of process
        static int persistent_fd = fd;
        (void)persistent_fd; // Suppress unused warning
        
        return false;
#endif
    }
    
#ifdef _WIN32
    // Windows-specific: Find and activate existing window
    static bool ActivateExistingInstance(const std::string& window_title) {
        HWND existing = FindWindowA(NULL, window_title.c_str());
        if (existing) {
            // Restore window if minimized
            if (IsIconic(existing)) {
                ShowWindow(existing, SW_RESTORE);
            }
            
            // Bring to foreground
            SetForegroundWindow(existing);
            return true;
        }
        return false;
    }
#endif
};

} // namespace lemon

