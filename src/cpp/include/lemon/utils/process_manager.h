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
        int timeout_seconds = -1);

    static void stop_process(ProcessHandle handle);
    static bool is_running(ProcessHandle handle);
    static int get_exit_code(ProcessHandle handle);
    static int wait_for_exit(ProcessHandle handle, int timeout_seconds = -1);
    static std::string read_output(ProcessHandle handle, int max_bytes = 4096);
    static void kill_process(ProcessHandle handle);

    // Replacement for system()/popen() that avoids console flashes in GUI apps.
    static int run_command(const std::string& command, std::string& output, int timeout_seconds = 30);

    static int find_free_port(int start_port = 8001);
};

} // namespace utils
} // namespace lemon
