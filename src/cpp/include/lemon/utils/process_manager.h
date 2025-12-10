#pragma once

#include <string>
#include <vector>
#include <functional>

namespace lemon {
namespace utils {

// Platform-independent process handle
struct ProcessHandle {
    void* handle;
    int pid;
};

// Callback for process output lines
// Returns: true to continue, false to kill the process
using OutputLineCallback = std::function<bool(const std::string& line)>;

class ProcessManager {
public:
    // Start a process with arguments
    static ProcessHandle start_process(
        const std::string& executable,
        const std::vector<std::string>& args,
        const std::string& working_dir = "",
        bool inherit_output = false,
        bool filter_health_logs = false,
        const std::vector<std::pair<std::string, std::string>>& env_vars = {});
    
    // Run a process and capture its output line by line
    // Blocks until process exits or callback returns false (which kills the process)
    // Returns: exit code, or -1 if killed by callback
    static int run_process_with_output(
        const std::string& executable,
        const std::vector<std::string>& args,
        OutputLineCallback on_line,
        const std::string& working_dir = "",
        int timeout_seconds = -1);
    
    // Stop a process
    static void stop_process(ProcessHandle handle);
    
    // Check if process is running
    static bool is_running(ProcessHandle handle);
    
    // Get process exit code (returns -1 if still running)
    static int get_exit_code(ProcessHandle handle);
    
    // Wait for process to exit
    static int wait_for_exit(ProcessHandle handle, int timeout_seconds = -1);
    
    // Read process output
    static std::string read_output(ProcessHandle handle, int max_bytes = 4096);
    
    // Kill process forcefully
    static void kill_process(ProcessHandle handle);
    
    // Find a free network port
    static int find_free_port(int start_port = 8001);
};

} // namespace utils
} // namespace lemon

