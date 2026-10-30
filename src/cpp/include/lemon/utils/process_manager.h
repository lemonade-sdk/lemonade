#pragma once

#include <string>
#include <vector>
#include <functional>

namespace lemon {
namespace utils {

struct ProcessHandle {
    void* handle;
    int pid;
};

// Returns true to continue, false to kill the process
using OutputLineCallback = std::function<bool(const std::string& line)>;

class ProcessManager {
public:
    static ProcessHandle start_process(
        const std::string& executable,
        const std::vector<std::string>& args,
        const std::string& working_dir = "",
        bool inherit_output = false,
        bool filter_health_logs = false,
        const std::vector<std::pair<std::string, std::string>>& env_vars = {});

    // Blocks until process exits or callback returns false (which kills the process)
    // Returns exit code, or -1 if killed by callback
    static int run_process_with_output(
        const std::string& executable,
        const std::vector<std::string>& args,
        OutputLineCallback on_line,
        const std::string& working_dir = "",
        int timeout_seconds = -1,
        bool capture_stderr = true);

    static void stop_process(ProcessHandle handle);

    // Must be non-mutating: on POSIX it must not reap the child, because
    // status/health checks may call this frequently.
    static bool is_running(ProcessHandle handle);

    // POSIX callers should prefer reap_process() when they intentionally own
    // cleanup of an exited child.
    static int get_exit_code(ProcessHandle handle);
    static int wait_for_exit(ProcessHandle handle, int timeout_seconds = -1);

    // Reap/close an already-exited child/handle and return its exit code.
    // Returns -1 if the process is still running, is not owned, or the status
    // cannot be obtained. This is intentionally explicit so liveness checks stay
    // read-only while lifecycle cleanup can reliably remove zombies.
    static int reap_process(ProcessHandle handle);

    static std::string read_output(ProcessHandle handle, int max_bytes = 4096);

    // Kill process forcefully and wait/close owned handles.
    static void kill_process(ProcessHandle handle);

    // Ask the OS to terminate the process without reaping or closing handles.
    // Use this only when another owner will perform the normal lifecycle cleanup.
    static void terminate_process(ProcessHandle handle);

    // Replacement for system()/popen() that avoids console flashes in GUI apps.
    static int run_command(const std::string& command, std::string& output, int timeout_seconds = 30);

    static int find_free_port(int start_port = 8001);
};

} // namespace utils
} // namespace lemon
