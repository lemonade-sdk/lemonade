// On Windows, set up header guards BEFORE any other includes
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
#endif

#include <lemon/utils/process_manager.h>
#include <lemon/utils/process_platform.h>
#include <stdexcept>
#include <iostream>
#include <thread>
#include <chrono>
#include <string>
#include <cstring>
#include <algorithm>
#include <cctype>
#include <lemon/utils/aixlog.hpp>

#ifdef _WIN32
#ifdef ERROR
#undef ERROR
#endif
#else
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#ifdef __linux__
#include <sys/prctl.h>  // PR_SET_PDEATHSIG — kill child when parent dies
#endif
#ifdef __APPLE__
#include <spawn.h>      // posix_spawn — fork-safe child creation on macOS
extern char** environ;
#endif
#ifdef HAVE_LIBCAP
#include <sys/capability.h>
#endif
#endif

namespace lemon {
namespace utils {

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

#ifdef HAVE_LIBCAP
// Helper function to preserve capabilities across exec()
// This allows child processes to inherit CAP_SYS_RESOURCE and other capabilities
// from the parent process when available
static void preserve_capabilities_for_exec() {
    // Get the current process capabilities
    cap_t caps = cap_get_proc();
    if (!caps) {
        // If we can't get capabilities, just proceed without them
        // This is not a fatal error - the process will run with default permissions
        return;
    }

    // Check if we have any effective capabilities worth preserving
    cap_flag_value_t has_sys_resource = CAP_CLEAR;
    cap_get_flag(caps, CAP_SYS_RESOURCE, CAP_EFFECTIVE, &has_sys_resource);

    // Only proceed if we actually have CAP_SYS_RESOURCE or other useful caps
    if (has_sys_resource == CAP_SET) {
        // Set the capability as inheritable so it survives exec()
        cap_value_t cap_list[] = {CAP_SYS_RESOURCE};

        // Mark CAP_SYS_RESOURCE as inheritable
        if (cap_set_flag(caps, CAP_INHERITABLE, 1, cap_list, CAP_SET) == 0) {
            // Apply the modified capability set
            if (cap_set_proc(caps) == 0) {
                // Enable ambient capabilities (requires Linux 4.3+)
                // This ensures the capability is both inherited and effective in the child
                // Ignore errors as ambient caps might not be supported on older kernels
                prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, CAP_SYS_RESOURCE, 0, 0);
            }
        }
    }

    cap_free(caps);
}
#endif


ProcessHandle ProcessManager::start_process(
    const std::string& executable,
    const std::vector<std::string>& args,
    const std::string& working_dir,
    bool inherit_output,
    bool filter_health_logs,
    const std::vector<std::pair<std::string, std::string>>& env_vars) {

    // Use platform abstraction on all platforms
    auto platform = create_process_platform();
    return platform->spawn(executable, args, working_dir, inherit_output, filter_health_logs, env_vars);

}

void ProcessManager::stop_process(ProcessHandle handle) {
    auto platform = create_process_platform();
    platform->terminate(handle);
}

bool ProcessManager::is_running(ProcessHandle handle) {
    auto platform = create_process_platform();
    return platform->is_running(handle);
}

int ProcessManager::get_exit_code(ProcessHandle handle) {
    auto platform = create_process_platform();
    return platform->get_exit_code(handle);
}

int ProcessManager::wait_for_exit(ProcessHandle handle, int timeout_seconds) {
    auto platform = create_process_platform();
    return platform->wait_for_exit(handle, timeout_seconds);
}

std::string ProcessManager::read_output(ProcessHandle handle, int max_bytes) {
    // Note: This is a simplified version. Full implementation would need pipes
    // for stdout/stderr capture during process creation
    return "";
}

int ProcessManager::run_process_with_output(
    const std::string& executable,
    const std::vector<std::string>& args,
    OutputLineCallback on_line,
    const std::string& working_dir,
    int timeout_seconds) {

    auto platform = create_process_platform();
    return platform->run_with_output(executable, args, on_line, working_dir, timeout_seconds);
}


void ProcessManager::kill_process(ProcessHandle handle) {
    auto platform = create_process_platform();
    platform->kill(handle);
}

int ProcessManager::find_free_port(int start_port) {
    auto platform = create_process_platform();
    return platform->find_free_port(start_port);
}

int ProcessManager::run_command(const std::string& command, std::string& output, int timeout_seconds) {
    auto platform = create_process_platform();
    return platform->run_command(command, output, timeout_seconds);
}

} // namespace utils
} // namespace lemon
