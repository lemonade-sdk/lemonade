#pragma once

#ifndef _WIN32

#include "lemon/utils/process_platform.h"
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace lemon::utils {

bool should_filter_process_line(const std::string& line);
bool is_error_process_line(const std::string& line);
void log_process_output_line(const std::string& line);

class UnixProcessPlatform : public ProcessPlatform {
public:
    ProcessHandle spawn(
        const std::string& executable,
        const std::vector<std::string>& args,
        const std::string& working_dir,
        bool inherit_output,
        bool filter_health_logs,
        const std::vector<std::pair<std::string, std::string>>& env_vars,
        const std::optional<lemon::sandbox::SandboxPolicy>& sandbox_policy = std::nullopt) override;

    void terminate(ProcessHandle handle) override;
    bool is_running(ProcessHandle handle) override;
    int get_exit_code(ProcessHandle handle) override;
    int wait_for_exit(ProcessHandle handle, int timeout_seconds) override;
    int reap(ProcessHandle handle) override;
    void kill(ProcessHandle handle) override;
    void terminate_without_cleanup(ProcessHandle handle) override;

    int run_with_output(
        const std::string& executable,
        const std::vector<std::string>& args,
        OutputLineCallback on_line,
        const std::string& working_dir,
        int timeout_seconds,
        bool capture_stderr = true) override;

    int find_free_port(int start_port) override;
    int run_command(const std::string& command, std::string& output, int timeout_seconds) override;

protected:
    virtual pid_t spawn_process(
        const std::string& executable,
        const std::vector<std::string>& args,
        const std::string& working_dir,
        bool inherit_output,
        bool filter_health_logs,
        const std::vector<std::pair<std::string, std::string>>& env_vars,
        int stdout_pipe[2],
        int stderr_pipe[2],
        const std::optional<lemon::sandbox::SandboxPolicy>& sandbox_policy = std::nullopt) = 0;

    virtual bool is_zombie(pid_t pid) const;
};

} // namespace lemon::utils

#endif
