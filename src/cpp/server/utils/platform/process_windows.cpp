// Windows header discipline — must precede all other includes.
// Mirrors the setup that was previously in process_manager.cpp before
// the platform files were made self-contained.
#ifdef _WIN32
#ifndef _WINSOCKAPI_
#define _WINSOCKAPI_
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include <processenv.h>
#pragma comment(lib, "ws2_32.lib")
// Un-define ERROR to avoid conflict with LOG(ERROR, ...) / SEVERITY::ERROR
#ifdef ERROR
#undef ERROR
#endif
#endif

#include <lemon/utils/aixlog.hpp>
#include <lemon/utils/process_platform.h>
#include <lemon/utils/windows_args.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cwchar>
#include <limits>
#include <map>
#include <stdexcept>
#include <thread>

namespace lemon {
namespace utils {

static std::wstring utf8_to_utf16(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    if (value.size() > static_cast<size_t>(std::numeric_limits<int>::max()) ||
        value.find('\0') != std::string::npos) {
        throw std::invalid_argument("Invalid UTF-8 process launch input");
    }
    const int length = static_cast<int>(value.size());
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                         value.data(), length, nullptr, 0);
    if (size == 0) {
        throw std::invalid_argument("Invalid UTF-8 process launch input");
    }
    std::wstring result(size, L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), length,
                            result.data(), size) != size) {
        throw std::runtime_error("Failed to convert process launch input to UTF-16");
    }
    return result;
}

// Helper function to check if a line should be filtered
static bool should_filter_line(const std::string& line) {
    // Filter out health check requests (both /health and /v1/health)
    // Also filter FLM's interactive prompt spam
    return (line.find("GET /health") != std::string::npos ||
            line.find("GET /v1/health") != std::string::npos ||
            // idle heartbeat returned by llamma cpp when its /metrics is scrapped. supressed to decrease visual clutering
            line.find("srv  update_slots: all slots are idle") != std::string::npos ||
            line.find("Enter 'exit' to stop the server") != std::string::npos);
}

static bool is_error_line(const std::string& line) {
    std::string lowered = line;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lowered.find("error") != std::string::npos;
}

// Helper function: filter and log process output
static void log_process_line(const std::string& line) {
    if (should_filter_line(line)) {
        return;
    }

    if (is_error_line(line)) {
        LOG(ERROR, "Process") << line << std::endl;
    } else {
        LOG(INFO, "Process") << line << std::endl;
    }
}

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

            log_process_line(line);
        }
    }

    // Print any remaining partial line
    if (!line_buffer.empty()) {
        log_process_line(line_buffer);
    }

    CloseHandle(pipe);
    return 0;
}

struct WindowsEnvironmentLess {
    bool operator()(const std::wstring& left, const std::wstring& right) const {
        return CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
                                    right.data(), static_cast<int>(right.size()),
                                    TRUE) == CSTR_LESS_THAN;
    }
};

static std::vector<wchar_t> build_windows_environment_block(
    const std::vector<std::pair<std::string, std::string>>& env_vars) {
    std::map<std::wstring, std::wstring, WindowsEnvironmentLess> merged_entries;
    std::vector<std::pair<std::wstring, std::wstring>> overrides;
    for (const auto& env : env_vars) {
        auto key = utf8_to_utf16(env.first);
        if (key.empty() || key.find(L'=') != std::wstring::npos) {
            throw std::invalid_argument("Invalid process environment variable name");
        }
        overrides.emplace_back(std::move(key), utf8_to_utf16(env.second));
    }

    LPWCH environment = GetEnvironmentStringsW();
    if (!environment) {
        throw std::runtime_error("Failed to read process environment");
    }
    try {
        for (const wchar_t* entry = environment; *entry != L'\0';
             entry += std::wcslen(entry) + 1) {
            const std::wstring value(entry);
            // Drive-current-directory entries start with '=' and must survive inheritance.
            const auto equals = value.find(L'=', value.front() == L'=' ? 1 : 0);
            if (equals != std::wstring::npos) {
                merged_entries[value.substr(0, equals)] = value.substr(equals + 1);
            }
        }
    } catch (...) {
        FreeEnvironmentStringsW(environment);
        throw;
    }
    FreeEnvironmentStringsW(environment);
    for (const auto& env : overrides) {
        merged_entries[env.first] = env.second;
    }

    std::vector<wchar_t> block;
    for (const auto& env : merged_entries) {
        block.insert(block.end(), env.first.begin(), env.first.end());
        block.push_back(L'=');
        block.insert(block.end(), env.second.begin(), env.second.end());
        block.push_back(L'\0');
    }
    if (block.empty()) {
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

// Windows ProcessPlatform implementation
class WindowsProcessPlatform : public ProcessPlatform {
public:
    ProcessHandle spawn(
        const std::string& executable,
        const std::vector<std::string>& args,
        const std::string& working_dir,
        bool inherit_output,
        bool filter_health_logs,
        const std::vector<std::pair<std::string, std::string>>& env_vars) override {

        ProcessHandle handle;
        handle.handle = nullptr;
        handle.pid = 0;

        std::string cmdline = escape_windows_arg(executable);
        for (const auto& arg : args) {
            cmdline += " " + escape_windows_arg(arg);
        }

        auto wide_cmdline = utf8_to_utf16(cmdline);
        const auto wide_working_dir = utf8_to_utf16(working_dir);
        std::vector<wchar_t> environment_block;
        if (!env_vars.empty()) {
            environment_block = build_windows_environment_block(env_vars);
        }
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        ZeroMemory(&pi, sizeof(pi));

        HANDLE stdout_read = nullptr;
        HANDLE stdout_write = nullptr;
        HANDLE stderr_read = nullptr;
        HANDLE stderr_write = nullptr;
        HANDLE nul_input = nullptr;

        bool use_filtered_output = (inherit_output && filter_health_logs);

        if (inherit_output && !filter_health_logs) {
            const HANDLE std_in = GetStdHandle(STD_INPUT_HANDLE);
            const HANDLE std_out = GetStdHandle(STD_OUTPUT_HANDLE);
            const HANDLE std_err = GetStdHandle(STD_ERROR_HANDLE);

            const bool invalid_stdio =
                (std_in == nullptr || std_in == INVALID_HANDLE_VALUE) ||
                (std_out == nullptr || std_out == INVALID_HANDLE_VALUE) ||
                (std_err == nullptr || std_err == INVALID_HANDLE_VALUE);

            if (invalid_stdio) {
                use_filtered_output = true;
                LOG(WARNING, "ProcessManager")
                    << "Parent std handles are unavailable; enabling filtered output capture"
                    << std::endl;
            }
        }

        // If inherit_output is true, either use pipes with filtering or direct inheritance
        if (inherit_output && use_filtered_output) {
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
            if (si.hStdInput == nullptr || si.hStdInput == INVALID_HANDLE_VALUE) {
                nul_input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (nul_input != INVALID_HANDLE_VALUE) {
                    SetHandleInformation(nul_input, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
                    si.hStdInput = nul_input;
                } else {
                    si.hStdInput = nullptr;
                }
            }
            si.hStdOutput = stdout_write;
            si.hStdError = stderr_write;

            LOG(DEBUG, "ProcessManager") << "Starting process with filtered output: " << cmdline << std::endl;
        } else if (inherit_output) {
            // Direct inheritance without filtering
            si.dwFlags |= STARTF_USESTDHANDLES;
            si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
            si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
            si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
            LOG(DEBUG, "ProcessManager") << "Starting process with inherited output: " << cmdline << std::endl;
        } else {
            // Redirect to NUL to suppress output when not in debug mode
            si.dwFlags |= STARTF_USESTDHANDLES;
            si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

            HANDLE hNul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hNul != INVALID_HANDLE_VALUE) {
                // Ensure the NUL handle is inheritable
                SetHandleInformation(hNul, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
                si.hStdOutput = hNul;
                si.hStdError = hNul;
            }
        }

        BOOL success = CreateProcessW(
            nullptr,
            wide_cmdline.data(),
            nullptr,
            nullptr,
            TRUE,  // Inherit handles
            CREATE_UNICODE_ENVIRONMENT | ((inherit_output && !use_filtered_output) ? 0 : CREATE_NO_WINDOW),
            environment_block.empty() ? nullptr : environment_block.data(),
            wide_working_dir.empty() ? nullptr : wide_working_dir.c_str(),
            &si,
            &pi
        );

        // If we opened a NUL handle, we can close it now (the child process has its own inherited handle)
        if (!inherit_output && si.hStdOutput != nullptr && si.hStdOutput != INVALID_HANDLE_VALUE) {
            CloseHandle(si.hStdOutput);
        }

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
            if (nul_input && nul_input != INVALID_HANDLE_VALUE) CloseHandle(nul_input);

            std::string full_error = "Failed to start process '" + executable +
                                    "': " + error_msg + " (Error code: " + std::to_string(error) + ")";
            LOG(ERROR, "ProcessManager") << full_error << std::endl;
            throw std::runtime_error(full_error);
        }

        if (nul_input && nul_input != INVALID_HANDLE_VALUE) {
            CloseHandle(nul_input);
        }

        // Close write ends of pipes in parent process
        if (stdout_write) CloseHandle(stdout_write);
        if (stderr_write) CloseHandle(stderr_write);

        // Start filter threads if needed
        if (inherit_output && use_filtered_output) {
            CreateThread(nullptr, 0, output_filter_thread, stdout_read, 0, nullptr);
            CreateThread(nullptr, 0, output_filter_thread, stderr_read, 0, nullptr);
        }

        if (inherit_output) {
            LOG(INFO, "ProcessManager") << "Process started successfully, PID: " << pi.dwProcessId << std::endl;
        }

        handle.handle = pi.hProcess;
        handle.pid = pi.dwProcessId;
        CloseHandle(pi.hThread);

        return handle;
    }

    void terminate(ProcessHandle handle) override {
        if (handle.handle) {
            TerminateProcess(handle.handle, 0);
            WaitForSingleObject(handle.handle, 5000);  // Wait up to 5 seconds
            CloseHandle(handle.handle);
        }
    }

    bool is_running(ProcessHandle handle) override {
        if (!handle.handle) {
            return false;
        }

        DWORD wait_result = WaitForSingleObject(handle.handle, 0);
        if (wait_result == WAIT_OBJECT_0) {
            return false;
        }
        if (wait_result == WAIT_TIMEOUT) {
            return true;
        }

        DWORD exit_code;
        if (!GetExitCodeProcess(handle.handle, &exit_code)) {
            return false;
        }

        return exit_code == STILL_ACTIVE;
    }

    int get_exit_code(ProcessHandle handle) override {
        if (!handle.handle) {
            return -1;
        }

        DWORD exit_code;
        if (!GetExitCodeProcess(handle.handle, &exit_code)) {
            return -1;
        }

        if (exit_code == STILL_ACTIVE) {
            return -1;
        }

        return static_cast<int>(exit_code);
    }

    int wait_for_exit(ProcessHandle handle, int timeout_seconds) override {
        if (!handle.handle) {
            return -1;
        }

        DWORD timeout_ms = (timeout_seconds < 0) ? INFINITE : (timeout_seconds * 1000);
        DWORD result = WaitForSingleObject(handle.handle, timeout_ms);

        if (result == WAIT_TIMEOUT) {
            return -1;
        }

        DWORD exit_code;
        GetExitCodeProcess(handle.handle, &exit_code);
        return exit_code;
    }

    int reap(ProcessHandle handle) override {
        if (!handle.handle) {
            return -1;
        }

        DWORD wait_result = WaitForSingleObject(handle.handle, 0);
        if (wait_result != WAIT_OBJECT_0) {
            return -1;
        }

        DWORD exit_code = STILL_ACTIVE;
        if (!GetExitCodeProcess(handle.handle, &exit_code)) {
            CloseHandle(handle.handle);
            return -1;
        }

        CloseHandle(handle.handle);
        return exit_code == STILL_ACTIVE ? -1 : static_cast<int>(exit_code);
    }

    void kill(ProcessHandle handle) override {
        if (handle.handle) {
            TerminateProcess(handle.handle, 1);
            CloseHandle(handle.handle);
        }
    }

    void terminate_without_cleanup(ProcessHandle handle) override {
        if (handle.handle) {
            TerminateProcess(handle.handle, 1);
        }
    }

    int run_with_output(
        const std::string& executable,
        const std::vector<std::string>& args,
        OutputLineCallback on_line,
        const std::string& working_dir,
        int timeout_seconds,
        bool capture_stderr = true) override {

        std::string cmdline = escape_windows_arg(executable);
        for (const auto& arg : args) {
            cmdline += " " + escape_windows_arg(arg);
        }

        auto wide_cmdline = utf8_to_utf16(cmdline);
        const auto wide_working_dir = utf8_to_utf16(working_dir);

        // Create pipes for stdout
        HANDLE stdout_read = nullptr;
        HANDLE stdout_write = nullptr;

        SECURITY_ATTRIBUTES sa;
        sa.nLength = sizeof(SECURITY_ATTRIBUTES);
        sa.bInheritHandle = TRUE;
        sa.lpSecurityDescriptor = nullptr;

        if (!CreatePipe(&stdout_read, &stdout_write, &sa, 0)) {
            throw std::runtime_error("Failed to create stdout pipe");
        }

        // Make sure the read handle is not inherited
        SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = stdout_write;
        si.hStdError = capture_stderr ? stdout_write : GetStdHandle(STD_ERROR_HANDLE);
        ZeroMemory(&pi, sizeof(pi));

        BOOL success = CreateProcessW(
            nullptr,
            wide_cmdline.data(),
            nullptr,
            nullptr,
            TRUE,  // Inherit handles
            CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
            nullptr,
            wide_working_dir.empty() ? nullptr : wide_working_dir.c_str(),
            &si,
            &pi
        );

        // Close write end in parent
        CloseHandle(stdout_write);

        if (!success) {
            CloseHandle(stdout_read);
            DWORD error = GetLastError();
            throw std::runtime_error("Failed to start process: error " + std::to_string(error));
        }

        // Read output line by line
        std::string line_buffer;
        char buffer[4096];
        DWORD bytes_read;
        bool killed_by_callback = false;

        auto start_time = std::chrono::steady_clock::now();

        while (true) {
            // Check timeout
            if (timeout_seconds > 0) {
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - start_time).count();
                if (elapsed > timeout_seconds) {
                    TerminateProcess(pi.hProcess, 1);
                    killed_by_callback = true;
                    break;
                }
            }

            // Check if there's data to read (non-blocking peek)
            DWORD available = 0;
            if (!PeekNamedPipe(stdout_read, nullptr, 0, nullptr, &available, nullptr)) {
                break;  // Pipe closed or error
            }

            if (available > 0) {
                DWORD to_read = (std::min)(available, (DWORD)(sizeof(buffer) - 1));
                if (ReadFile(stdout_read, buffer, to_read, &bytes_read, nullptr) && bytes_read > 0) {
                    buffer[bytes_read] = '\0';
                    line_buffer += buffer;

                    // Process complete lines (split on \n or \r for in-place progress updates)
                    size_t pos;
                    while (true) {
                        // Find the first line terminator (\n or \r)
                        size_t newline_pos = line_buffer.find('\n');
                        size_t cr_pos = line_buffer.find('\r');

                        if (newline_pos == std::string::npos && cr_pos == std::string::npos) {
                            break;  // No complete line yet
                        }

                        // Use whichever comes first
                        if (newline_pos == std::string::npos) {
                            pos = cr_pos;
                        } else if (cr_pos == std::string::npos) {
                            pos = newline_pos;
                        } else {
                            pos = (std::min)(newline_pos, cr_pos);
                        }

                        std::string line = line_buffer.substr(0, pos);

                        // Skip \r\n as a single delimiter
                        size_t skip = 1;
                        if (pos + 1 < line_buffer.size() &&
                            line_buffer[pos] == '\r' && line_buffer[pos + 1] == '\n') {
                            skip = 2;
                        }
                        line_buffer = line_buffer.substr(pos + skip);

                        // Skip empty lines
                        if (line.empty()) {
                            continue;
                        }

                        // Call the callback
                        if (on_line && !on_line(line)) {
                            TerminateProcess(pi.hProcess, 1);
                            killed_by_callback = true;
                            break;
                        }
                    }

                    if (killed_by_callback) break;
                }
            } else {
                // No data available, check if process is still running
                DWORD exit_code;
                if (GetExitCodeProcess(pi.hProcess, &exit_code) && exit_code != STILL_ACTIVE) {
                    // Process exited, drain any remaining output
                    while (ReadFile(stdout_read, buffer, sizeof(buffer) - 1, &bytes_read, nullptr) && bytes_read > 0) {
                        buffer[bytes_read] = '\0';
                        line_buffer += buffer;
                    }
                    break;
                }

                // Sleep briefly to avoid busy-waiting
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }

        // Process any remaining partial line
        if (!line_buffer.empty() && on_line && !killed_by_callback) {
            // Remove trailing \r if present
            if (!line_buffer.empty() && line_buffer.back() == '\r') {
                line_buffer.pop_back();
            }
            if (!line_buffer.empty()) {
                on_line(line_buffer);
            }
        }

        CloseHandle(stdout_read);

        // Get exit code
        DWORD exit_code = 0;
        WaitForSingleObject(pi.hProcess, 5000);
        GetExitCodeProcess(pi.hProcess, &exit_code);

        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        return killed_by_callback ? -1 : static_cast<int>(exit_code);
    }

    int find_free_port(int start_port) override {
        WSADATA wsa_data;
        WSAStartup(MAKEWORD(2, 2), &wsa_data);

        for (int port = start_port; port < start_port + 1000; ++port) {
            // Test if port is free by attempting to bind to localhost
            int sock = socket(AF_INET, SOCK_STREAM, 0);
            if (sock < 0) {
                continue;
            }

            sockaddr_in addr;
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            addr.sin_addr.s_addr = inet_addr("127.0.0.1");

            int result = bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
            closesocket(sock);

            if (result == 0) {
                WSACleanup();
                return port;
            }
        }

        WSACleanup();
        return -1;
    }

    int run_command(const std::string& command, std::string& output, int timeout_seconds) override {
        output.clear();
        auto wide_cmdline = utf8_to_utf16("cmd /c " + command);

        // Windows: use CreateProcess + pipe to avoid console window flash.
        // This is a drop-in replacement for _popen() that works in SUBSYSTEM:WINDOWS apps.
        HANDLE stdout_read = nullptr;
        HANDLE stdout_write = nullptr;

        SECURITY_ATTRIBUTES sa;
        sa.nLength = sizeof(SECURITY_ATTRIBUTES);
        sa.bInheritHandle = TRUE;
        sa.lpSecurityDescriptor = nullptr;

        if (!CreatePipe(&stdout_read, &stdout_write, &sa, 0)) {
            return -1;
        }
        SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW si = {};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = INVALID_HANDLE_VALUE;
        si.hStdOutput = stdout_write;
        si.hStdError = stdout_write;

        PROCESS_INFORMATION pi = {};
        // Wrap in cmd /c so shell features (redirection, pipes) work
        BOOL success = CreateProcessW(
            nullptr, wide_cmdline.data(),
            nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
            nullptr, nullptr, &si, &pi);

        CloseHandle(stdout_write);

        if (!success) {
            CloseHandle(stdout_read);
            return -1;
        }

        // Read all output
        char buf[4096];
        DWORD bytes_read;
        while (ReadFile(stdout_read, buf, sizeof(buf), &bytes_read, nullptr) && bytes_read > 0) {
            output.append(buf, bytes_read);
        }
        CloseHandle(stdout_read);

        WaitForSingleObject(pi.hProcess, timeout_seconds > 0 ? timeout_seconds * 1000 : INFINITE);
        DWORD exit_code = 1;
        GetExitCodeProcess(pi.hProcess, &exit_code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        return static_cast<int>(exit_code);
    }
};

// Factory function
std::unique_ptr<ProcessPlatform> create_process_platform() {
    return std::make_unique<WindowsProcessPlatform>();
}

} // namespace utils
} // namespace lemon
