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
#ifdef _WIN32
    if (handle.handle) {
        TerminateProcess(handle.handle, 0);
        WaitForSingleObject(handle.handle, 5000);  // Wait up to 5 seconds
        CloseHandle(handle.handle);
    }
#else
    if (handle.pid > 0) {
#ifdef WNOWAIT
        // If the child has already exited, reap it without sending a signal.
        // If it is no longer our child, do not signal the PID; it may have been
        // reaped already and the numeric PID could be stale or reused.
        siginfo_t info;
        std::memset(&info, 0, sizeof(info));
        if (waitid(P_PID, static_cast<id_t>(handle.pid), &info, WEXITED | WNOHANG | WNOWAIT) == 0) {
            if (info.si_pid != 0) {
                reap_process(handle);
                LOG(INFO, "ProcessManager") << "Process already exited; reaped PID "
                                             << handle.pid << std::endl;
                LOG(INFO, "ProcessManager") << "Process terminated, waiting for GPU driver cleanup..." << std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(2));
                return;
            }
        } else if (errno == ECHILD) {
            LOG(WARNING, "ProcessManager") << "Process PID " << handle.pid
                                           << " is no longer an owned child; skipping termination"
                                           << std::endl;
            return;
        }
#endif

        if (kill(handle.pid, SIGTERM) != 0 && errno == ESRCH) {
            LOG(INFO, "ProcessManager") << "Process PID " << handle.pid
                                        << " was already gone before SIGTERM" << std::endl;
            return;
        }

        // Wait for process to exit
        int status = 0;
        bool exited_gracefully = false;
        for (int i = 0; i < 50; i++) {  // Try for 5 seconds
            pid_t result = waitpid(handle.pid, &status, WNOHANG);
            if (result > 0) {
                exited_gracefully = true;
                break;
            }
            if (result < 0 && errno == ECHILD) {
                exited_gracefully = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        if (!exited_gracefully) {
            // If still alive, force kill. Avoid signaling if the process vanished
            // after the wait loop.
            LOG(WARNING, "ProcessManager") << "Process did not respond to SIGTERM, using SIGKILL" << std::endl;
            if (kill(handle.pid, SIGKILL) == 0 || errno != ESRCH) {
                waitpid(handle.pid, &status, 0);
            }
        }

        // CRITICAL FIX: GPU drivers need time to release Vulkan/ROCm contexts
        // The process may exit but GPU resources persist briefly in the kernel driver.
        // Without this delay, rapid restarts cause the new process to hang waiting
        // for GPU resources that are still being cleaned up.
        // This matches the Python test behavior which has a 5s delay after server start.
        LOG(INFO, "ProcessManager") << "Process terminated, waiting for GPU driver cleanup..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
#endif
}

bool ProcessManager::is_running(ProcessHandle handle) {
#ifdef _WIN32
    if (!handle.handle) {
        return false;
    }

    // A Windows process object becomes signaled when the process exits. This is
    // the closest equivalent to a cheap child-exit check on POSIX and avoids
    // relying only on STILL_ACTIVE from GetExitCodeProcess.
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
#else
    if (handle.pid <= 0) {
        return false;
    }

#ifdef WNOWAIT
    // Observe child state without reaping it. Reaping in a cheap liveness check
    // loses the exit status for later lifecycle/error reporting and makes status
    // endpoints mutate process state. waitid(..., WNOWAIT) reports exited
    // children while leaving them waitable for explicit lifecycle cleanup.
    siginfo_t info;
    std::memset(&info, 0, sizeof(info));
    if (waitid(P_PID, static_cast<id_t>(handle.pid), &info, WEXITED | WNOHANG | WNOWAIT) == 0) {
        return info.si_pid == 0;
    }
    if (errno == ECHILD) {
        // The PID is no longer an owned child. Treat it as not running instead
        // of falling back to kill(pid, 0), which could report true for a reused
        // PID and make a stale backend look alive.
        return false;
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

int ProcessManager::reap_process(ProcessHandle handle) {
#ifdef _WIN32
    if (!handle.handle) {
        return -1;
    }

    // Do not close an active process handle here. The caller still owns the
    // lifecycle for running children, and closing the only handle would make
    // later termination/reaping impossible.
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
#else
    if (handle.pid <= 0) {
        return -1;
    }

    int status = 0;
    pid_t result = waitpid(handle.pid, &status, WNOHANG);
    if (result <= 0) {
        return -1;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return -1;
#endif
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
