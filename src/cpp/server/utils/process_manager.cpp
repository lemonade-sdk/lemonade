#include <lemon/utils/process_manager.h>
#include <stdexcept>
#include <iostream>
#include <thread>
#include <chrono>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

namespace lemon {
namespace utils {

// Helper function to check if a line should be filtered
static bool should_filter_line(const std::string& line) {
    // Filter out health check requests (both /health and /v1/health)
    return (line.find("GET /health") != std::string::npos ||
            line.find("GET /v1/health") != std::string::npos);
}

#ifdef _WIN32
// Thread function to read from pipe and filter output
static DWORD WINAPI output_filter_thread(LPVOID param) {
    HANDLE pipe = static_cast<HANDLE>(param);
    char buffer[4096];
    DWORD bytes_read;
    std::string line_buffer;
    
    while (ReadFile(pipe, buffer, sizeof(buffer) - 1, &bytes_read, nullptr) && bytes_read > 0) {
        buffer[bytes_read] = '\0';
        line_buffer += buffer;
        
        // Process complete lines
        size_t pos;
        while ((pos = line_buffer.find('\n')) != std::string::npos) {
            std::string line = line_buffer.substr(0, pos);
            line_buffer = line_buffer.substr(pos + 1);
            
            // Only print if not a health check line
            if (!should_filter_line(line)) {
                std::cout << line << std::endl;
            }
        }
    }
    
    // Print any remaining partial line
    if (!line_buffer.empty() && !should_filter_line(line_buffer)) {
        std::cout << line_buffer << std::endl;
    }
    
    CloseHandle(pipe);
    return 0;
}
#endif

ProcessHandle ProcessManager::start_process(
    const std::string& executable,
    const std::vector<std::string>& args,
    const std::string& working_dir,
    bool inherit_output,
    bool filter_health_logs,
    const std::vector<std::pair<std::string, std::string>>& env_vars) {
    
    ProcessHandle handle;
    handle.handle = nullptr;
    handle.pid = 0;
    
#ifdef _WIN32
    // Windows implementation
    std::string cmdline = "\"" + executable + "\"";
    for (const auto& arg : args) {
        cmdline += " \"" + arg + "\"";
    }
    
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));
    
    HANDLE stdout_read = nullptr;
    HANDLE stdout_write = nullptr;
    HANDLE stderr_read = nullptr;
    HANDLE stderr_write = nullptr;
    
    // If inherit_output is true, either use pipes with filtering or direct inheritance
    if (inherit_output && filter_health_logs) {
        // Create pipes for stdout and stderr to filter output
        SECURITY_ATTRIBUTES sa;
        sa.nLength = sizeof(SECURITY_ATTRIBUTES);
        sa.bInheritHandle = TRUE;
        sa.lpSecurityDescriptor = nullptr;
        
        if (!CreatePipe(&stdout_read, &stdout_write, &sa, 0)) {
            throw std::runtime_error("Failed to create stdout pipe");
        }
        if (!CreatePipe(&stderr_read, &stderr_write, &sa, 0)) {
            CloseHandle(stdout_read);
            CloseHandle(stdout_write);
            throw std::runtime_error("Failed to create stderr pipe");
        }
        
        // Make sure the read handles are not inherited
        SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0);
        
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = stdout_write;
        si.hStdError = stderr_write;
        
        std::cout << "[ProcessManager] Starting process with filtered output: " << cmdline << std::endl;
    } else if (inherit_output) {
        // Direct inheritance without filtering
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        std::cout << "[ProcessManager] Starting process with inherited output: " << cmdline << std::endl;
    } else {
        std::cout << "[ProcessManager] Starting process: " << cmdline << std::endl;
    }
    
    BOOL success = CreateProcessA(
        nullptr,
        const_cast<char*>(cmdline.c_str()),
        nullptr,
        nullptr,
        TRUE,  // Inherit handles
        (inherit_output && !filter_health_logs) ? 0 : CREATE_NO_WINDOW,
        nullptr,
        working_dir.empty() ? nullptr : working_dir.c_str(),
        &si,
        &pi
    );
    
    if (!success) {
        DWORD error = GetLastError();
        char error_msg[256];
        FormatMessageA(
            FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr,
            error,
            0,
            error_msg,
            sizeof(error_msg),
            nullptr
        );
        
        if (stdout_write) CloseHandle(stdout_write);
        if (stderr_write) CloseHandle(stderr_write);
        if (stdout_read) CloseHandle(stdout_read);
        if (stderr_read) CloseHandle(stderr_read);
        
        std::string full_error = "Failed to start process '" + executable + 
                                "': " + error_msg + " (Error code: " + std::to_string(error) + ")";
        std::cerr << "[ProcessManager ERROR] " << full_error << std::endl;
        throw std::runtime_error(full_error);
    }
    
    // Close write ends of pipes in parent process
    if (stdout_write) CloseHandle(stdout_write);
    if (stderr_write) CloseHandle(stderr_write);
    
    // Start filter threads if needed
    if (inherit_output && filter_health_logs) {
        CreateThread(nullptr, 0, output_filter_thread, stdout_read, 0, nullptr);
        CreateThread(nullptr, 0, output_filter_thread, stderr_read, 0, nullptr);
    }
    
    std::cout << "[ProcessManager] Process started successfully, PID: " << pi.dwProcessId << std::endl;
    
    handle.handle = pi.hProcess;
    handle.pid = pi.dwProcessId;
    CloseHandle(pi.hThread);
    
#else
    // Unix implementation
    pid_t pid = fork();
    
    if (pid < 0) {
        throw std::runtime_error("Failed to fork process");
    }
    
    if (pid == 0) {
        // Child process
        if (!working_dir.empty()) {
            chdir(working_dir.c_str());
        }
        
        // Set environment variables
        for (const auto& env_pair : env_vars) {
            setenv(env_pair.first.c_str(), env_pair.second.c_str(), 1);
        }
        
        // Prepare argv
        std::vector<char*> argv_ptrs;
        argv_ptrs.push_back(const_cast<char*>(executable.c_str()));
        for (const auto& arg : args) {
            argv_ptrs.push_back(const_cast<char*>(arg.c_str()));
        }
        argv_ptrs.push_back(nullptr);
        
        execvp(executable.c_str(), argv_ptrs.data());
        
        // If execvp returns, it failed
        std::cerr << "Failed to execute: " << executable << std::endl;
        exit(1);
    }
    
    // Parent process
    handle.pid = pid;
    
#endif
    
    return handle;
}

void ProcessManager::stop_process(ProcessHandle handle) {
#ifdef _WIN32
    if (handle.handle) {
        TerminateProcess(handle.handle, 0);
        WaitForSingleObject(handle.handle, 5000);  // Wait up to 5 seconds
        CloseHandle(handle.handle);
    }
#else
    if (handle.pid > 0) {
        kill(handle.pid, SIGTERM);
        
        // Wait for process to exit
        int status;
        for (int i = 0; i < 50; i++) {  // Try for 5 seconds
            if (waitpid(handle.pid, &status, WNOHANG) > 0) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        // If still alive, force kill
        kill(handle.pid, SIGKILL);
        waitpid(handle.pid, &status, 0);
    }
#endif
}

bool ProcessManager::is_running(ProcessHandle handle) {
#ifdef _WIN32
    if (!handle.handle) {
        return false;
    }
    
    DWORD exit_code;
    if (!GetExitCodeProcess(handle.handle, &exit_code)) {
        return false;
    }
    
    return exit_code == STILL_ACTIVE;
#else
    if (handle.pid <= 0) {
        return false;
    }
    
    int status;
    pid_t result = waitpid(handle.pid, &status, WNOHANG);
    return result == 0;  // 0 means still running
#endif
}

int ProcessManager::get_exit_code(ProcessHandle handle) {
#ifdef _WIN32
    if (!handle.handle) {
        return -1;
    }
    
    DWORD exit_code;
    if (!GetExitCodeProcess(handle.handle, &exit_code)) {
        return -1;
    }
    
    if (exit_code == STILL_ACTIVE) {
        return -1;  // Still running
    }
    
    return static_cast<int>(exit_code);
#else
    if (handle.pid <= 0) {
        return -1;
    }
    
    int status;
    pid_t result = waitpid(handle.pid, &status, WNOHANG);
    
    if (result == 0) {
        return -1;  // Still running
    }
    
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    
    return -1;
#endif
}

int ProcessManager::wait_for_exit(ProcessHandle handle, int timeout_seconds) {
#ifdef _WIN32
    if (!handle.handle) {
        return -1;
    }
    
    DWORD wait_time = timeout_seconds < 0 ? INFINITE : timeout_seconds * 1000;
    DWORD result = WaitForSingleObject(handle.handle, wait_time);
    
    if (result == WAIT_TIMEOUT) {
        return -1;
    }
    
    DWORD exit_code;
    GetExitCodeProcess(handle.handle, &exit_code);
    return exit_code;
#else
    if (handle.pid <= 0) {
        return -1;
    }
    
    int status;
    if (timeout_seconds < 0) {
        waitpid(handle.pid, &status, 0);
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    
    for (int i = 0; i < timeout_seconds * 10; i++) {
        pid_t result = waitpid(handle.pid, &status, WNOHANG);
        if (result > 0) {
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    return -1;  // Timeout
#endif
}

std::string ProcessManager::read_output(ProcessHandle handle, int max_bytes) {
    // Note: This is a simplified version. Full implementation would need pipes
    // for stdout/stderr capture during process creation
    return "";
}

void ProcessManager::kill_process(ProcessHandle handle) {
#ifdef _WIN32
    if (handle.handle) {
        TerminateProcess(handle.handle, 1);
        CloseHandle(handle.handle);
    }
#else
    if (handle.pid > 0) {
        kill(handle.pid, SIGKILL);
        int status;
        waitpid(handle.pid, &status, 0);
    }
#endif
}

int ProcessManager::find_free_port(int start_port) {
#ifdef _WIN32
    WSADATA wsa_data;
    WSAStartup(MAKEWORD(2, 2), &wsa_data);
#endif
    
    for (int port = start_port; port < start_port + 1000; port++) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            continue;
        }
        
        sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = INADDR_ANY;
        
        int result = bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        
#ifdef _WIN32
        closesocket(sock);
#else
        close(sock);
#endif
        
        if (result == 0) {
#ifdef _WIN32
            WSACleanup();
#endif
            return port;
        }
    }
    
#ifdef _WIN32
    WSACleanup();
#endif
    
    return -1;  // No free port found
}

} // namespace utils
} // namespace lemon

