#pragma once

#include <lemon/utils/process_manager.h>
#include <string>
#include <vector>
#include <memory>

namespace lemon::utils {

// Abstract interface for platform-specific process operations
class ProcessPlatform {
public:
    virtual ~ProcessPlatform() = default;

    // Process lifecycle
    virtual ProcessHandle spawn(
        const std::string& executable,
        const std::vector<std::string>& args,
        const std::string& working_dir,
        bool inherit_output,
        bool filter_health_logs,
        const std::vector<std::pair<std::string, std::string>>& env_vars) = 0;

    virtual void terminate(ProcessHandle handle) = 0;
    virtual bool is_running(ProcessHandle handle) = 0;
    virtual int get_exit_code(ProcessHandle handle) = 0;
    virtual int wait_for_exit(ProcessHandle handle, int timeout_seconds) = 0;
    virtual void kill(ProcessHandle handle) = 0;

    // Explicit lifecycle cleanup for watchdog/reset paths.
    //
    // Default implementations keep older platform files source-compatible while
    // updated platform implementations provide the correct OS-specific behavior.
    virtual int reap(ProcessHandle handle) {
        return get_exit_code(handle);
    }

    virtual void terminate_without_cleanup(ProcessHandle handle) = 0;

    // Process with output callback
    virtual int run_with_output(
        const std::string& executable,
        const std::vector<std::string>& args,
        OutputLineCallback on_line,
        const std::string& working_dir,
        int timeout_seconds,
        bool capture_stderr = true) = 0;

    // Utility functions
    virtual int find_free_port(int start_port) = 0;
    virtual int run_command(const std::string& command, std::string& output, int timeout_seconds) = 0;
};

// Factory function to create platform-specific implementation
std::unique_ptr<ProcessPlatform> create_process_platform();

} // namespace lemon::utils
