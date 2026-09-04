#ifndef _WIN32

#include "process_unix_base.h"
#include <lemon/utils/aixlog.hpp>

#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <chrono>
#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace lemon::utils {

bool should_filter_process_line(const std::string& line) {
    return (line.find("GET /health") != std::string::npos ||
            line.find("GET /v1/health") != std::string::npos ||
            line.find("srv  update_slots: all slots are idle") != std::string::npos ||
            line.find("Enter 'exit' to stop the server") != std::string::npos);
}

bool is_error_process_line(const std::string& line) {
    std::string lowered = line;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lowered.find("error") != std::string::npos;
}

void log_process_output_line(const std::string& line) {
    if (should_filter_process_line(line)) {
        return;
    }

    if (is_error_process_line(line)) {
        LOG(ERROR, "Process") << line << std::endl;
    } else {
        LOG(INFO, "Process") << line << std::endl;
    }
}

bool UnixProcessPlatform::is_zombie(pid_t) const {
    return false;
}

ProcessHandle UnixProcessPlatform::spawn(
    const std::string& executable,
    const std::vector<std::string>& args,
    const std::string& working_dir,
    bool inherit_output,
    bool filter_health_logs,
    const std::vector<std::pair<std::string, std::string>>& env_vars,
    const std::optional<lemon::sandbox::SandboxPolicy>& sandbox_policy) {

    int stdout_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};

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
                              filter_health_logs, env_vars, stdout_pipe, stderr_pipe,
                              sandbox_policy);

    if (pid <= 0) {
        if (inherit_output && filter_health_logs) {
            close(stdout_pipe[0]);
            close(stdout_pipe[1]);
            close(stderr_pipe[0]);
            close(stderr_pipe[1]);
        }
        return {nullptr, 0};
    }

    if (inherit_output) {
        LOG(INFO, "ProcessManager") << "Process started successfully, PID: " << pid << std::endl;
    }

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
                    log_process_output_line(line);
                }
            }

            if (!line_buffer.empty()) {
                log_process_output_line(line_buffer);
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
                    log_process_output_line(line);
                }
            }

            if (!line_buffer.empty()) {
                log_process_output_line(line_buffer);
            }

            close(fd);
        }).detach();
    }

    return {reinterpret_cast<void*>(static_cast<uintptr_t>(pid)), pid};
}

void UnixProcessPlatform::terminate(ProcessHandle handle) {
    if (handle.pid <= 0) {
        return;
    }

    ::kill(handle.pid, SIGTERM);

    for (int i = 0; i < 50; ++i) {
        if (!is_running(handle)) {
            reap(handle);
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (is_running(handle)) {
        ::kill(handle.pid, SIGKILL);
        reap(handle);
    }
}

bool UnixProcessPlatform::is_running(ProcessHandle handle) {
    if (handle.pid <= 0) {
        return false;
    }

    if (::kill(handle.pid, 0) == -1) {
        if (errno == ESRCH) {
            return false;
        }
        if (errno == EPERM) {
            return true;
        }
    }

    if (is_zombie(handle.pid)) {
        return false;
    }

    return true;
}

int UnixProcessPlatform::get_exit_code(ProcessHandle handle) {
    if (handle.pid <= 0) {
        return -1;
    }

    int status = 0;
    pid_t result = waitpid(handle.pid, &status, WNOHANG);

    if (result == handle.pid) {
        if (WIFEXITED(status)) {
            return WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            return 128 + WTERMSIG(status);
        }
    } else if (result == -1 && errno == ECHILD) {
        return -1;
    }

    return -1;
}

int UnixProcessPlatform::wait_for_exit(ProcessHandle handle, int timeout_seconds) {
    if (handle.pid <= 0) {
        return -1;
    }

    auto start_time = std::chrono::steady_clock::now();

    while (true) {
        int status = 0;
        pid_t result = waitpid(handle.pid, &status, WNOHANG);

        if (result == handle.pid) {
            if (WIFEXITED(status)) {
                return WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
                return 128 + WTERMSIG(status);
            }
            return 0;
        } else if (result == -1 && errno == ECHILD) {
            return -1;
        }

        if (timeout_seconds >= 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start_time).count();
            if (elapsed >= timeout_seconds) {
                return -1;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

int UnixProcessPlatform::reap(ProcessHandle handle) {
    if (handle.pid <= 0) {
        return -1;
    }

    int status = 0;
    pid_t result = waitpid(handle.pid, &status, WNOHANG);
    if (result == handle.pid) {
        if (WIFEXITED(status)) {
            return WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            return 128 + WTERMSIG(status);
        }
        return 0;
    }
    return -1;
}

void UnixProcessPlatform::kill(ProcessHandle handle) {
    if (handle.pid <= 0) {
        return;
    }

    ::kill(handle.pid, SIGKILL);
    reap(handle);
}

void UnixProcessPlatform::terminate_without_cleanup(ProcessHandle handle) {
    if (handle.pid <= 0) {
        return;
    }

    ::kill(handle.pid, SIGTERM);
}

int UnixProcessPlatform::run_with_output(
    const std::string& executable,
    const std::vector<std::string>& args,
    OutputLineCallback on_line,
    const std::string& working_dir,
    int timeout_seconds,
    bool capture_stderr) {

    int stdout_pipe[2];
    int stderr_pipe[2];

    if (pipe(stdout_pipe) < 0) {
        return -1;
    }
    if (capture_stderr && pipe(stderr_pipe) < 0) {
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        return -1;
    }

    pid_t pid = spawn_process(executable, args, working_dir, false, false,
                              {}, stdout_pipe, capture_stderr ? stderr_pipe : stdout_pipe);

    if (pid <= 0) {
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        if (capture_stderr) {
            close(stderr_pipe[0]);
            close(stderr_pipe[1]);
        }
        return -1;
    }

    close(stdout_pipe[1]);
    if (capture_stderr) {
        close(stderr_pipe[1]);
    }

    auto start_time = std::chrono::steady_clock::now();
    bool killed_by_callback = false;

    auto process_stream = [&](int fd) {
        char buffer[1024];
        std::string line_buffer;

        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        while (true) {
            if (timeout_seconds > 0) {
                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count() > timeout_seconds) {
                    ::kill(pid, SIGKILL);
                    break;
                }
            }

            ssize_t bytes_read = read(fd, buffer, sizeof(buffer) - 1);
            if (bytes_read > 0) {
                buffer[bytes_read] = '\0';
                line_buffer += buffer;

                size_t pos;
                while ((pos = line_buffer.find('\n')) != std::string::npos) {
                    std::string line = line_buffer.substr(0, pos);
                    line_buffer.erase(0, pos + 1);

                    if (!line.empty() && line.back() == '\r') {
                        line.pop_back();
                    }

                    if (on_line && !on_line(line)) {
                        killed_by_callback = true;
                        ::kill(pid, SIGKILL);
                        return;
                    }
                }
            } else if (bytes_read == 0) {
                break;
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    int status;
                    if (waitpid(pid, &status, WNOHANG) > 0) {
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                } else {
                    break;
                }
            }
        }

        if (!line_buffer.empty() && on_line) {
            on_line(line_buffer);
        }

        close(fd);
    };

    process_stream(stdout_pipe[0]);
    if (capture_stderr) {
        process_stream(stderr_pipe[0]);
    }

    if (killed_by_callback) {
        int status;
        waitpid(pid, &status, 0);
        return -1;
    }

    int status;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }

    return -1;
}

int UnixProcessPlatform::find_free_port(int start_port) {
    for (int port = start_port; port < 65535; ++port) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            continue;
        }

        int opt = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        addr.sin_port = htons(port);

        if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
            close(sock);
            return port;
        }

        close(sock);
    }
    return -1;
}

int UnixProcessPlatform::run_command(const std::string& command, std::string& output, int timeout_seconds) {
    FILE* pipe_file = popen(command.c_str(), "r");
    if (!pipe_file) {
        return -1;
    }

    char buffer[256];
    output.clear();

    auto start_time = std::chrono::steady_clock::now();

    while (fgets(buffer, sizeof(buffer), pipe_file) != nullptr) {
        output += buffer;

        if (timeout_seconds > 0) {
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count() > timeout_seconds) {
                pclose(pipe_file);
                return -1;
            }
        }
    }

    int status = pclose(pipe_file);
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }

    return -1;
}

} // namespace lemon::utils

#endif
