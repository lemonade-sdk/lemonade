#include <lemon/utils/process_manager.h>
#include <lemon/utils/process_platform.h>

namespace lemon {
namespace utils {

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

#ifdef WNOWAIT
    // Observe child state without reaping it. Reaping in a cheap liveness check
    // loses the exit status for later lifecycle/error reporting and makes status
    // endpoints mutate process state. waitid(..., WNOWAIT) reports exited
    // children while leaving them waitable for get_exit_code()/wait_for_exit().
    siginfo_t info;
    std::memset(&info, 0, sizeof(info));
    if (waitid(P_PID, static_cast<id_t>(handle.pid), &info, WEXITED | WNOHANG | WNOWAIT) == 0) {
        return info.si_pid == 0;
    }
#endif

    // Fallback for platforms without WNOWAIT: kill(pid, 0) does not signal the
    // process and does not reap. It cannot distinguish a zombie from a running
    // child, but it is still safer than waitpid(WNOHANG) for read-only checks.
    return kill(handle.pid, 0) == 0 || errno == EPERM;
#endif
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

void ProcessManager::terminate_process(ProcessHandle handle) {
#ifdef _WIN32
    if (handle.handle) {
        TerminateProcess(handle.handle, 1);
    }
#else
    if (handle.pid > 0) {
        kill(handle.pid, SIGKILL);
    }
#endif
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
