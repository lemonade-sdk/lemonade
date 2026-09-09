// Windows header discipline: must precede all other includes.
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

#include <lemon/utils/process_platform.h>
#include <lemon/utils/aixlog.hpp>
#include <lemon/sandbox/env_scrubber.h>
#include <lemon/sandbox/windows_security_utils.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <stdexcept>
#include <unordered_map>

namespace lemon {
namespace utils {

struct WindowsProcessLifetime {
    std::unique_ptr<lemon::sandbox::AclGrantGuard> acl_guard;
    HANDLE job_handle{nullptr};
    PSID loopback_sid{nullptr};
};

static std::mutex g_process_lifetime_mutex;
static std::unordered_map<HANDLE, WindowsProcessLifetime> g_process_lifetimes;

static void cleanup_process_lifetime(HANDLE hProcess) {
    if (!hProcess) return;
    std::unique_ptr<lemon::sandbox::AclGrantGuard> acl_guard;
    HANDLE job_handle = nullptr;
    PSID loopback_sid = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_process_lifetime_mutex);
        auto it = g_process_lifetimes.find(hProcess);
        if (it != g_process_lifetimes.end()) {
            acl_guard = std::move(it->second.acl_guard);
            job_handle = it->second.job_handle;
            loopback_sid = it->second.loopback_sid;
            g_process_lifetimes.erase(it);
        }
    }
    if (loopback_sid) {
        lemon::sandbox::WindowsSecurityUtils::set_appcontainer_loopback_exemption(loopback_sid, false);
        lemon::sandbox::WindowsSecurityUtils::free_appcontainer_sid(loopback_sid);
    }
    if (job_handle) {
        CloseHandle(job_handle);
    }
    // acl_guard destructor runs here, revoking ACEs on termination
}

static std::wstring utf8_to_wstring(const std::string& str) {
    if (str.empty()) return std::wstring();
    int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
    if (size <= 0) return std::wstring();
    std::wstring result(size - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &result[0], size);
    return result;
}

static std::string escape_windows_arg(const std::string& arg) {
    if (arg.empty()) {
        return "\"\"";
    }
    if (arg.find_first_of(" \t\n\v\"") == std::string::npos) {
        return arg;
    }
    std::string result = "\"";
    for (size_t i = 0; i < arg.size(); ++i) {
        if (arg[i] == '"') {
            // Escape the quote with a backslash
            result += "\\\"";
        } else if (arg[i] == '\\') {
            // Check if this backslash is followed by a quote
            // If so, we need to escape the backslash too
            if (i + 1 < arg.size() && arg[i + 1] == '"') {
                result += "\\\\";
            } else {
                result += '\\';
            }
        } else {
            result += arg[i];
        }
    }
    result += "\"";
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

// Helper function: lowercase ASCII string for case-insensitive comparison
static std::string lowercase_ascii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Helper function: build Windows environment block
static std::vector<char> build_windows_environment_block(
    const std::vector<std::pair<std::string, std::string>>& env_vars) {
    std::vector<std::string> merged_entries;

    LPWCH environment = GetEnvironmentStringsW();
    if (environment) {
        for (const wchar_t* entry = environment; *entry != L'\0';
             entry += std::wcslen(entry) + 1) {
            int size_needed = WideCharToMultiByte(CP_UTF8, 0, entry, -1, nullptr, 0, nullptr, nullptr);
            if (size_needed > 0) {
                std::string narrow(size_needed - 1, '\0');
                WideCharToMultiByte(CP_UTF8, 0, entry, -1, &narrow[0], size_needed, nullptr, nullptr);
                merged_entries.emplace_back(std::move(narrow));
            }
        }
        FreeEnvironmentStringsW(environment);
    }

    for (const auto& env : env_vars) {
        const std::string key_lower = lowercase_ascii(env.first);
        const std::string new_entry = env.first + "=" + env.second;

        bool replaced = false;
        for (auto& existing : merged_entries) {
            size_t equals = existing.find('=');
            if (equals == std::string::npos) {
                continue;
            }

            std::string existing_key = lowercase_ascii(existing.substr(0, equals));
            if (existing_key == key_lower) {
                existing = new_entry;
                replaced = true;
                break;
            }
        }

        if (!replaced) {
            merged_entries.push_back(new_entry);
        }
    }

    std::vector<char> block;
    for (const auto& entry : merged_entries) {
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back('\0');
    }
    block.push_back('\0');
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
        const std::vector<std::pair<std::string, std::string>>& env_vars,
        const std::optional<lemon::sandbox::SandboxPolicy>& sandbox_policy = std::nullopt) override {

        ProcessHandle handle;
        handle.handle = nullptr;
        handle.pid = 0;

        std::string cmdline = escape_windows_arg(executable);
        for (const auto& arg : args) {
            cmdline += " " + escape_windows_arg(arg);
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
                nul_input = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
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
            if (si.hStdInput == nullptr || si.hStdInput == INVALID_HANDLE_VALUE) {
                nul_input = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (nul_input != INVALID_HANDLE_VALUE) {
                    SetHandleInformation(nul_input, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
                    si.hStdInput = nul_input;
                }
            }

            HANDLE hNul = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hNul != INVALID_HANDLE_VALUE) {
                // Ensure the NUL handle is inheritable
                SetHandleInformation(hNul, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
                si.hStdOutput = hNul;
                si.hStdError = hNul;
                stdout_write = hNul;
            }
        }

        std::vector<char> environment_block;
        const bool should_sanitize = sandbox_policy.has_value() &&
                                     sandbox_policy->mode != lemon::sandbox::SandboxMode::Disabled;
        if (should_sanitize) {
            std::vector<std::pair<std::string, std::string>> combined_env = env_vars;
            for (const auto& kv : sandbox_policy->explicit_env_vars) {
                combined_env.push_back(kv);
            }
            auto sanitized_env = lemon::sandbox::EnvScrubber::sanitize_environment(
                combined_env, sandbox_policy->allowed_env_vars, true);

            for (const auto& entry : sanitized_env) {
                std::string line = entry.first + "=" + entry.second;
                environment_block.insert(environment_block.end(), line.begin(), line.end());
                environment_block.push_back('\0');
            }
            if (!environment_block.empty()) {
                environment_block.push_back('\0');
            }
        } else {
            environment_block = build_windows_environment_block(env_vars);
        }

        BOOL success = FALSE;
        std::unique_ptr<lemon::sandbox::AclGrantGuard> acl_guard;
        HANDLE hSandboxedJob = nullptr;
        PSID app_container_sid = nullptr;

        if (sandbox_policy.has_value() &&
            sandbox_policy->mode != lemon::sandbox::SandboxMode::Disabled) {

            std::filesystem::path exec_path(executable);
            std::wstring backend_ident = exec_path.stem().wstring();
            std::wstring container_name = L"Lemonade.Backend." + backend_ident;
            app_container_sid = lemon::sandbox::WindowsSecurityUtils::derive_appcontainer_sid(container_name);

            if (app_container_sid != nullptr) {
                if (sandbox_policy->network_access != lemon::sandbox::NetworkAccess::DenyAll) {
                    lemon::sandbox::WindowsSecurityUtils::set_appcontainer_loopback_exemption(app_container_sid, true);
                }

                acl_guard = std::make_unique<lemon::sandbox::AclGrantGuard>(
                    app_container_sid, sandbox_policy->path_grants);

                hSandboxedJob = lemon::sandbox::WindowsSecurityUtils::create_sandboxed_job_object(false);

                std::wstring cmdline_w = utf8_to_wstring(cmdline);
                std::wstring working_dir_w = utf8_to_wstring(working_dir);

                STARTUPINFOEXW siEx = {};
                siEx.StartupInfo.cb = sizeof(STARTUPINFOEXW);
                siEx.StartupInfo.dwFlags = si.dwFlags;
                siEx.StartupInfo.hStdInput = si.hStdInput;
                siEx.StartupInfo.hStdOutput = si.hStdOutput;
                siEx.StartupInfo.hStdError = si.hStdError;

                SIZE_T attr_size = 0;
                InitializeProcThreadAttributeList(nullptr, 4, 0, &attr_size);
                std::vector<BYTE> attr_buffer(attr_size);
                siEx.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buffer.data());

                if (InitializeProcThreadAttributeList(siEx.lpAttributeList, 4, 0, &attr_size)) {
                    SECURITY_CAPABILITIES secCaps = {};
                    secCaps.AppContainerSid = app_container_sid;
                    secCaps.Capabilities = nullptr;
                    secCaps.CapabilityCount = 0;

                    BOOL attr_ok = UpdateProcThreadAttribute(siEx.lpAttributeList, 0,
                        PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES,
                        &secCaps, sizeof(secCaps), nullptr, nullptr);

                    DWORD64 mitigations = lemon::sandbox::WindowsSecurityUtils::get_standard_backend_mitigations();
                    if (mitigations != 0 && attr_ok) {
                        attr_ok = UpdateProcThreadAttribute(siEx.lpAttributeList, 0,
                            PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY,
                            &mitigations, sizeof(mitigations), nullptr, nullptr);
                    }

                    if (hSandboxedJob != nullptr && attr_ok) {
                        attr_ok = UpdateProcThreadAttribute(siEx.lpAttributeList, 0,
                            PROC_THREAD_ATTRIBUTE_JOB_LIST,
                            &hSandboxedJob, sizeof(HANDLE), nullptr, nullptr);
                    }

                    std::vector<HANDLE> inherit_handles;
                    if (si.hStdInput && si.hStdInput != INVALID_HANDLE_VALUE) inherit_handles.push_back(si.hStdInput);
                    if (si.hStdOutput && si.hStdOutput != INVALID_HANDLE_VALUE) inherit_handles.push_back(si.hStdOutput);
                    if (si.hStdError && si.hStdError != INVALID_HANDLE_VALUE) inherit_handles.push_back(si.hStdError);

                    if (!inherit_handles.empty() && attr_ok) {
                        attr_ok = UpdateProcThreadAttribute(siEx.lpAttributeList, 0,
                            PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                            inherit_handles.data(), inherit_handles.size() * sizeof(HANDLE), nullptr, nullptr);
                    }

                    if (attr_ok) {
                        success = CreateProcessW(
                            nullptr,
                            const_cast<LPWSTR>(cmdline_w.c_str()),
                            nullptr,
                            nullptr,
                            TRUE,
                            EXTENDED_STARTUPINFO_PRESENT | ((inherit_output && !use_filtered_output) ? 0 : CREATE_NO_WINDOW),
                            environment_block.empty() ? nullptr : environment_block.data(),
                            working_dir_w.empty() ? nullptr : working_dir_w.c_str(),
                            &siEx.StartupInfo,
                            &pi
                        );
                    }

                    DeleteProcThreadAttributeList(siEx.lpAttributeList);
                }

                if (!success) {
                    lemon::sandbox::WindowsSecurityUtils::free_appcontainer_sid(app_container_sid);
                    app_container_sid = nullptr;
                }
            }
        }

        if (!success) {
            if (sandbox_policy.has_value() && sandbox_policy->mode == lemon::sandbox::SandboxMode::Enforced) {
                LOG(ERROR, "ProcessPlatform")
                    << "Enforced AppContainer sandboxing failed for " << executable
                    << "; refusing to spawn unconfined." << std::endl;
                if (hSandboxedJob) CloseHandle(hSandboxedJob);
                if (stdout_read) CloseHandle(stdout_read);
                if (stdout_write) CloseHandle(stdout_write);
                if (stderr_read) CloseHandle(stderr_read);
                if (stderr_write) CloseHandle(stderr_write);
                if (nul_input && nul_input != INVALID_HANDLE_VALUE) CloseHandle(nul_input);
                return handle;
            }

            // Fallback unconfined spawn
            LOG(WARNING, "ProcessPlatform")
                << "AppContainer sandboxing unavailable for " << executable
                << "; falling back to unconfined execution with environment scrubbing." << std::endl;

            std::string cmdline_a = cmdline;
            std::string working_dir_a = working_dir;

            success = CreateProcessA(
                nullptr,
                const_cast<LPSTR>(cmdline_a.c_str()),
                nullptr,
                nullptr,
                TRUE,
                ((inherit_output && !use_filtered_output) ? 0 : CREATE_NO_WINDOW),
                environment_block.empty() ? nullptr : environment_block.data(),
                working_dir_a.empty() ? nullptr : working_dir_a.c_str(),
                &si,
                &pi
            );
        }

        if (!success) {
            DWORD error = GetLastError();
            LOG(ERROR, "ProcessManager") << "Failed to start process (error " + std::to_string(error) + "): " + executable << std::endl;

            if (hSandboxedJob) CloseHandle(hSandboxedJob);
            if (stdout_read) CloseHandle(stdout_read);
            if (stdout_write) CloseHandle(stdout_write);
            if (stderr_read) CloseHandle(stderr_read);
            if (stderr_write) CloseHandle(stderr_write);
            if (nul_input && nul_input != INVALID_HANDLE_VALUE) CloseHandle(nul_input);

            return handle;
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

        if (acl_guard != nullptr || hSandboxedJob != nullptr || app_container_sid != nullptr) {
            std::lock_guard<std::mutex> lock(g_process_lifetime_mutex);
            g_process_lifetimes[pi.hProcess] = {std::move(acl_guard), hSandboxedJob, app_container_sid};
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
            cleanup_process_lifetime(handle.handle);
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
            cleanup_process_lifetime(handle.handle);
            CloseHandle(handle.handle);
            return -1;
        }

        cleanup_process_lifetime(handle.handle);
        CloseHandle(handle.handle);
        return exit_code == STILL_ACTIVE ? -1 : static_cast<int>(exit_code);
    }

    void kill(ProcessHandle handle) override {
        if (handle.handle) {
            TerminateProcess(handle.handle, 1);
            cleanup_process_lifetime(handle.handle);
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

        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = stdout_write;
        si.hStdError = capture_stderr ? stdout_write : GetStdHandle(STD_ERROR_HANDLE);
        ZeroMemory(&pi, sizeof(pi));

        BOOL success = CreateProcessA(
            nullptr,
            const_cast<char*>(cmdline.c_str()),
            nullptr,
            nullptr,
            TRUE,  // Inherit handles
            CREATE_NO_WINDOW,
            nullptr,
            working_dir.empty() ? nullptr : working_dir.c_str(),
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

        STARTUPINFOA si = {};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = INVALID_HANDLE_VALUE;
        si.hStdOutput = stdout_write;
        si.hStdError = stdout_write;

        PROCESS_INFORMATION pi = {};
        // Wrap in cmd /c so shell features (redirection, pipes) work
        std::string cmdline = "cmd /c " + command;
        BOOL success = CreateProcessA(
            nullptr, const_cast<char*>(cmdline.c_str()),
            nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
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
