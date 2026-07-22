#include <lemon/utils/process_platform.h>
#include <lemon/utils/aixlog.hpp>

#include <stdexcept>
#include <iostream>
#include <fstream>

#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#include <cstring>
#include <thread>
#include <chrono>
#include <algorithm>
#include <cctype>

#include <sys/prctl.h>

#ifdef HAVE_LIBCAP
#include <sys/capability.h>
#endif

namespace lemon::utils {

// Helper functions
static bool should_filter_line(const std::string& line) {
    return (line.find("GET /health") != std::string::npos ||
            line.find("GET /v1/health") != std::string::npos ||
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
static void preserve_capabilities_for_exec() {
    cap_t caps = cap_get_proc();
    if (!caps) {
        return;
    }

    cap_flag_value_t has_sys_resource = CAP_CLEAR;
    cap_get_flag(caps, CAP_SYS_RESOURCE, CAP_EFFECTIVE, &has_sys_resource);

    if (has_sys_resource == CAP_SET) {
        cap_value_t cap_list[] = {CAP_SYS_RESOURCE};

        if (cap_set_flag(caps, CAP_INHERITABLE, 1, cap_list, CAP_SET) == 0) {
            if (cap_set_proc(caps) == 0) {
                prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, CAP_SYS_RESOURCE, 0, 0);
            }
        }
    }

    cap_free(caps);
}
#endif

static bool is_zombie_by_proc(pid_t pid) {
    std::ifstream stat_file("/proc/" + std::to_string(pid) + "/stat");
    std::string stat_line;
    if (!std::getline(stat_file, stat_line)) {
        return false;
    }

    // Process state is the first field after the final ')' of comm.
    const auto close_paren = stat_line.rfind(')');
    return close_paren != std::string::npos &&
           close_paren + 2 < stat_line.size() &&
           stat_line[close_paren + 2] == 'Z';
}

class LinuxProcessPlatform : public ProcessPlatform {
public:
    ProcessHandle spawn(
        const std::string& executable,
        const std::vector<std::string>& args,
        const std::string& working_dir,
        bool inherit_output,
        bool filter_health_logs,
        const std::vector<std::pair<std::string, std::string>>& env_vars) override;

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
    // To be overridden by platform-specific subclasses (macOS uses posix_spawn)
    virtual pid_t spawn_process(
        const std::string& executable,
        const std::vector<std::string>& args,
        const std::string& working_dir,
        bool inherit_output,
        bool filter_health_logs,
        const std::vector<std::pair<std::string, std::string>>& env_vars,
        int stdout_pipe[2],
        int stderr_pipe[2]);
};

// Linux implementation using fork/exec
pid_t LinuxProcessPlatform::spawn_process(
    const std::string& executable,
    const std::vector<std::string>& args,
    const std::string& working_dir,
    bool inherit_output,
    bool filter_health_logs,
    const std::vector<std::pair<std::string, std::string>>& env_vars,
    int stdout_pipe[2],
    int stderr_pipe[2]) {

    pid_t pid = fork();

    if (pid < 0) {
        throw std::runtime_error("Failed to fork process");
    }

    if (pid == 0) {
        // Child process
        prctl(PR_SET_PDEATHSIG, SIGTERM);

        if (!working_dir.empty()) {
            chdir(working_dir.c_str());
        }

        // Set environment variables
        for (const auto& env_pair : env_vars) {
            setenv(env_pair.first.c_str(), env_pair.second.c_str(), 1);
        }

        // Redirect stdout/stderr to pipes if filtering
        if (inherit_output && filter_health_logs) {
            close(stdout_pipe[0]);
            close(stderr_pipe[0]);
            dup2(stdout_pipe[1], STDOUT_FILENO);
            dup2(stderr_pipe[1], STDERR_FILENO);
            close(stdout_pipe[1]);
            close(stderr_pipe[1]);
        } else if (!inherit_output) {
            int dev_null = open("/dev/null", O_WRONLY);
            if (dev_null >= 0) {
                dup2(dev_null, STDOUT_FILENO);
                dup2(dev_null, STDERR_FILENO);
                close(dev_null);
            }
        }

#ifdef HAVE_LIBCAP
        preserve_capabilities_for_exec();
#endif

        // Prepare argv
        std::vector<char*> argv_ptrs;
        argv_ptrs.push_back(const_cast<char*>(executable.c_str()));
        for (const auto& arg : args) {
            argv_ptrs.push_back(const_cast<char*>(arg.c_str()));
        }
        argv_ptrs.push_back(nullptr);

        execvp(executable.c_str(), argv_ptrs.data());

        // If execvp returns, it failed
        std::cerr << "Failed to execute: " << executable << std::endl;
        _exit(1);
    }

    return pid;
}

ProcessHandle LinuxProcessPlatform::spawn(
    const std::string& executable,
    const std::vector<std::string>& args,
    const std::string& working_dir,
    bool inherit_output,
    bool filter_health_logs,
    const std::vector<std::pair<std::string, std::string>>& env_vars) {

    ProcessHandle handle;
    handle.handle = nullptr;
    handle.pid = 0;

    int stdout_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};

    // Create pipes for filtering if requested
    if (inherit_output && filter_health_logs) {
        if (pipe(stdout_pipe) < 0 || pipe(stderr_pipe) < 0) {
            throw std::runtime_error("Failed to create pipes for output filtering");
        }
    }

    if (inherit_output) {
        std::string cmdline = executable;
        for (const auto& arg : args) {
            cmdline += " " + arg;
        }
        if (filter_health_logs) {
            LOG(DEBUG, "ProcessManager") << "Starting process with filtered output: " << cmdline << std::endl;
        } else {
            LOG(DEBUG, "ProcessManager") << "Starting process with inherited output: " << cmdline << std::endl;
        }
    }

    pid_t pid = spawn_process(executable, args, working_dir, inherit_output,
                              filter_health_logs, env_vars, stdout_pipe, stderr_pipe);

    handle.pid = pid;

    if (inherit_output) {
        LOG(INFO, "ProcessManager") << "Process started successfully, PID: " << pid << std::endl;
    }

    // Start filter threads if needed
    if (inherit_output && filter_health_logs) {
        close(stdout_pipe[1]);
        close(stderr_pipe[1]);

        std::thread([fd = stdout_pipe[0]]() {
            char buffer[4096];
            std::string line_buffer;
            ssize_t bytes_read;

            while ((bytes_read = read(fd, buffer, sizeof(buffer) - 1)) > 0) {
                buffer[bytes_read] = '\0';
                line_buffer += buffer;

                size_t pos;
                while ((pos = line_buffer.find('\n')) != std::string::npos) {
                    std::string line = line_buffer.substr(0, pos);
                    line_buffer = line_buffer.substr(pos + 1);
                    log_process_line(line);
                }
            }

            if (!line_buffer.empty()) {
                log_process_line(line_buffer);
            }

            close(fd);
        }).detach();

        std::thread([fd = stderr_pipe[0]]() {
            char buffer[4096];
            std::string line_buffer;
            ssize_t bytes_read;

            while ((bytes_read = read(fd, buffer, sizeof(buffer) - 1)) > 0) {
                buffer[bytes_read] = '\0';
                line_buffer += buffer;

                size_t pos;
                while ((pos = line_buffer.find('\n')) != std::string::npos) {
                    std::string line = line_buffer.substr(0, pos);
                    line_buffer = line_buffer.substr(pos + 1);
                    log_process_line(line);
                }
            }

            if (!line_buffer.empty()) {
                log_process_line(line_buffer);
            }

            close(fd);
        }).detach();
    }

    return handle;
}

void LinuxProcessPlatform::terminate(ProcessHandle handle) {
    if (handle.pid <= 0) {
        return;
    }

#ifdef WNOWAIT
    siginfo_t info;
    std::memset(&info, 0, sizeof(info));
    if (waitid(P_PID, static_cast<id_t>(handle.pid), &info, WEXITED | WNOHANG | WNOWAIT) == 0) {
        if (info.si_pid != 0) {
            reap(handle);
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

    errno = 0;
    if (::kill(handle.pid, SIGTERM) != 0 && errno == ESRCH) {
        LOG(INFO, "ProcessManager") << "Process PID " << handle.pid
                                    << " was already gone before SIGTERM" << std::endl;
        return;
    }

    int status = 0;
    bool exited_gracefully = false;
    for (int i = 0; i < 50; ++i) {
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
        LOG(WARNING, "ProcessManager") << "Process did not respond to SIGTERM, using SIGKILL" << std::endl;
        errno = 0;
        if (::kill(handle.pid, SIGKILL) == 0 || errno != ESRCH) {
            waitpid(handle.pid, &status, 0);
        }
    }

    LOG(INFO, "ProcessManager") << "Process terminated, waiting for GPU driver cleanup..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(2));
}

bool LinuxProcessPlatform::is_running(ProcessHandle handle) {
    if (handle.pid <= 0) {
        return false;
    }

#ifdef WNOWAIT
    siginfo_t info;
    std::memset(&info, 0, sizeof(info));
    if (waitid(P_PID, static_cast<id_t>(handle.pid), &info, WEXITED | WNOHANG | WNOWAIT) == 0) {
        return info.si_pid == 0;
    }

    if (errno == ECHILD) {
        return false;
    }
#endif

    if (is_zombie_by_proc(handle.pid)) {
        return false;
    }

    errno = 0;
    return ::kill(handle.pid, 0) == 0 || errno == EPERM;
}

int LinuxProcessPlatform::get_exit_code(ProcessHandle handle) {
    if (handle.pid <= 0) {
        return -1;
    }

    int status = 0;
    pid_t result = waitpid(handle.pid, &status, WNOHANG);

    if (result == 0) {
        return -1;
    }

    if (result < 0) {
        return -1;
    }

    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }

    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }

    return -1;
}

int LinuxProcessPlatform::wait_for_exit(ProcessHandle handle, int timeout_seconds) {
    if (handle.pid <= 0) {
        return -1;
    }

    int status = 0;
    if (timeout_seconds < 0) {
        if (waitpid(handle.pid, &status, 0) <= 0) {
            return -1;
        }
        if (WIFEXITED(status)) {
            return WEXITSTATUS(status);
        }
        if (WIFSIGNALED(status)) {
            return 128 + WTERMSIG(status);
        }
        return -1;
    }

    for (int i = 0; i < timeout_seconds * 10; ++i) {
        pid_t result = waitpid(handle.pid, &status, WNOHANG);
        if (result > 0) {
            if (WIFEXITED(status)) {
                return WEXITSTATUS(status);
            }
            if (WIFSIGNALED(status)) {
                return 128 + WTERMSIG(status);
            }
            return -1;
        }
        if (result < 0) {
            return -1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    return -1;
}

int LinuxProcessPlatform::reap(ProcessHandle handle) {
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
}

void LinuxProcessPlatform::kill(ProcessHandle handle) {
    if (handle.pid > 0) {
        errno = 0;
        if (::kill(handle.pid, SIGKILL) == 0 || errno != ESRCH) {
            int status = 0;
            waitpid(handle.pid, &status, 0);
        }
    }
}

void LinuxProcessPlatform::terminate_without_cleanup(ProcessHandle handle) {
    if (handle.pid > 0) {
        ::kill(handle.pid, SIGKILL);
    }
}

int LinuxProcessPlatform::run_with_output(
    const std::string& executable,
    const std::vector<std::string>& args,
    OutputLineCallback on_line,
    const std::string& working_dir,
    int timeout_seconds,
    bool capture_stderr) {

    int stdout_pipe[2];

    if (pipe(stdout_pipe) < 0) {
        throw std::runtime_error("Failed to create pipe");
    }

    pid_t pid = fork();

    if (pid < 0) {
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        throw std::runtime_error("Failed to fork process");
    }

    if (pid == 0) {
        // Child process
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        close(stdout_pipe[0]);

        dup2(stdout_pipe[1], STDOUT_FILENO);
        if (capture_stderr) {
            dup2(stdout_pipe[1], STDERR_FILENO);
        }
        close(stdout_pipe[1]);

        if (!working_dir.empty()) {
            chdir(working_dir.c_str());
        }

#ifdef HAVE_LIBCAP
        preserve_capabilities_for_exec();
#endif

        std::vector<char*> argv_ptrs;
        argv_ptrs.push_back(const_cast<char*>(executable.c_str()));
        for (const auto& arg : args) {
            argv_ptrs.push_back(const_cast<char*>(arg.c_str()));
        }
        argv_ptrs.push_back(nullptr);

        execvp(executable.c_str(), argv_ptrs.data());
        _exit(127);
    }

    // Parent process
    close(stdout_pipe[1]);

    std::string line_buffer;
    char buffer[4096];
    ssize_t bytes_read;
    bool killed_by_callback = false;

    auto start_time = std::chrono::steady_clock::now();

    int flags = fcntl(stdout_pipe[0], F_GETFL, 0);
    fcntl(stdout_pipe[0], F_SETFL, flags | O_NONBLOCK);

    while (true) {
        if (timeout_seconds > 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start_time).count();
            if (elapsed > timeout_seconds) {
                ::kill(pid, SIGKILL);
                killed_by_callback = true;
                break;
            }
        }

        bytes_read = read(stdout_pipe[0], buffer, sizeof(buffer) - 1);

        if (bytes_read > 0) {
            buffer[bytes_read] = '\0';
            line_buffer += buffer;

            size_t pos;
            while (true) {
                size_t newline_pos = line_buffer.find('\n');
                size_t cr_pos = line_buffer.find('\r');

                if (newline_pos == std::string::npos && cr_pos == std::string::npos) {
                    break;
                }

                if (newline_pos == std::string::npos) {
                    pos = cr_pos;
                } else if (cr_pos == std::string::npos) {
                    pos = newline_pos;
                } else {
                    pos = std::min(newline_pos, cr_pos);
                }

                std::string line = line_buffer.substr(0, pos);

                size_t skip = 1;
                if (pos + 1 < line_buffer.size() &&
                    line_buffer[pos] == '\r' && line_buffer[pos + 1] == '\n') {
                    skip = 2;
                }
                line_buffer = line_buffer.substr(pos + skip);

                if (line.empty()) {
                    continue;
                }

                if (on_line && !on_line(line)) {
                    ::kill(pid, SIGKILL);
                    killed_by_callback = true;
                    break;
                }
            }

            if (killed_by_callback) break;
        } else if (bytes_read == 0) {
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                int status = 0;
                pid_t result = waitpid(pid, &status, WNOHANG);
                if (result > 0) {
                    fcntl(stdout_pipe[0], F_SETFL, flags);
                    while ((bytes_read = read(stdout_pipe[0], buffer, sizeof(buffer) - 1)) > 0) {
                        buffer[bytes_read] = '\0';
                        line_buffer += buffer;
                    }
                    break;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            } else {
                break;
            }
        }
    }

    if (!line_buffer.empty() && on_line && !killed_by_callback) {
        on_line(line_buffer);
    }

    close(stdout_pipe[0]);

    int status = 0;
    waitpid(pid, &status, 0);

    if (killed_by_callback) {
        return -1;
    }

    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int LinuxProcessPlatform::find_free_port(int start_port) {
    for (int port = start_port; port < start_port + 1000; ++port) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            continue;
        }

        sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");

        int result = bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        close(sock);

        if (result == 0) {
            return port;
        }
    }

    return -1;
}

int LinuxProcessPlatform::run_command(const std::string& command, std::string& output, int timeout_seconds) {
    output.clear();

    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        return -1;
    }

    char buf[4096];
    while (fgets(buf, sizeof(buf), pipe)) {
        output += buf;
    }

    return pclose(pipe);
}

std::unique_ptr<ProcessPlatform> create_process_platform() {
    return std::make_unique<LinuxProcessPlatform>();
}

} // namespace lemon::utils
