#include <lemon/utils/process_manager.h>
#include <lemon/utils/process_platform.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
#include <lemon/utils/windows_args.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace lemon {
namespace utils {

ProcessHandle ProcessManager::start_process(
    const std::string& executable,
    const std::vector<std::string>& args,
    const std::string& working_dir,
    bool inherit_output,
    bool filter_health_logs,
    const std::vector<std::pair<std::string, std::string>>& env_vars) {

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

int ProcessManager::reap_process(ProcessHandle handle) {
    auto platform = create_process_platform();
    return platform->reap(handle);
}

CapturedProcess ProcessManager::capture_output(const std::string& executable, const std::vector<std::string>& args,
    std::size_t max_bytes, unsigned timeout_ms, const std::function<bool()>& cancelled, CaptureOutput output) {
    if (executable.empty() || executable.find('\0') != std::string::npos || !max_bytes || max_bytes > 65536 || !timeout_ms || timeout_ms > 5000)
        throw std::invalid_argument("Invalid bounded process capture policy");
    for (const auto& arg : args) if (arg.find('\0') != std::string::npos) throw std::invalid_argument("Invalid process argument");
    CapturedProcess result;
    std::exception_ptr cancellation_error;
    const auto is_cancelled = [&] {
        try { return cancelled && cancelled(); }
        catch (...) { cancellation_error = std::current_exception(); return true; }
    };
    if (is_cancelled()) {
        if (cancellation_error) std::rethrow_exception(cancellation_error);
        result.cancelled = true;
        return result;
    }
    result.output.reserve(max_bytes);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
#ifdef _WIN32
    const auto wide = [](const std::string& value) {
        const auto length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
        if (length <= 0) throw std::invalid_argument("Invalid UTF-8 process argument");
        std::wstring result(static_cast<std::size_t>(length), L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), length);
        return result;
    };
    auto arguments = escape_windows_arg(executable);
    for (const auto& arg : args) arguments += " " + escape_windows_arg(arg);
    auto command = wide(arguments);
    const auto program = wide(executable);
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE reader = nullptr, writer = nullptr;
    if (!CreatePipe(&reader, &writer, &security, 0)) throw std::runtime_error("Capture pipe failed");
    SetHandleInformation(reader, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = writer;
    const auto null_error = output == CaptureOutput::Stdout ? CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr) : INVALID_HANDLE_VALUE;
    if (output == CaptureOutput::Stdout && null_error == INVALID_HANDLE_VALUE) {
        CloseHandle(reader); CloseHandle(writer); throw std::runtime_error("Capture stderr setup failed");
    }
    startup.hStdError = output == CaptureOutput::Stdout ? null_error : writer;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    const auto job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        if (job) CloseHandle(job);
        if (null_error != INVALID_HANDLE_VALUE) CloseHandle(null_error);
        CloseHandle(reader); CloseHandle(writer); throw std::runtime_error("Capture job setup failed");
    }
    const bool created = CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup, &process);
    CloseHandle(writer);
    if (null_error != INVALID_HANDLE_VALUE) CloseHandle(null_error);
    if (!created) { CloseHandle(job); CloseHandle(reader); throw std::runtime_error("Capture spawn failed"); }
    if (!AssignProcessToJobObject(job, process.hProcess)) {
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hThread); CloseHandle(process.hProcess); CloseHandle(job); CloseHandle(reader);
        throw std::runtime_error("Capture process confinement failed");
    }
    ResumeThread(process.hThread);
    CloseHandle(process.hThread);
    bool exited = false, eof = false;
#else
    int pipes[2];
    if (pipe(pipes) != 0) throw std::runtime_error("Capture pipe failed");
    if (fcntl(pipes[0], F_SETFD, FD_CLOEXEC) < 0 || fcntl(pipes[1], F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(pipes[0], F_SETFL, O_NONBLOCK) < 0) {
        close(pipes[0]); close(pipes[1]); throw std::runtime_error("Capture pipe setup failed");
    }
    std::vector<char*> argv{const_cast<char*>(executable.c_str())};
    for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipes[1], STDOUT_FILENO);
    if (output == CaptureOutput::Combined) posix_spawn_file_actions_adddup2(&actions, pipes[1], STDERR_FILENO);
    else posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addclose(&actions, pipes[0]);
    posix_spawn_file_actions_addclose(&actions, pipes[1]);
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attributes, 0);
    pid_t pid = -1;
    const auto spawned = posix_spawn(&pid, executable.c_str(), &actions, &attributes, argv.data(), environ);
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    close(pipes[1]);
    if (spawned != 0) { close(pipes[0]); throw std::runtime_error("Capture spawn failed"); }
    bool exited = false, eof = false;
    int status = 0;
#endif
    while (true) {
        result.cancelled = is_cancelled();
        result.timed_out = std::chrono::steady_clock::now() >= deadline;
        if (result.cancelled || result.timed_out || result.byte_limit) break;
        char buffer[4096];
        const auto capacity = std::min(sizeof(buffer), max_bytes - result.output.size() + 1);
        std::size_t bytes = 0;
#ifdef _WIN32
        DWORD available = 0, received = 0;
        if (!PeekNamedPipe(reader, nullptr, 0, nullptr, &available, nullptr)) eof = true;
        else if (available && ReadFile(reader, buffer, static_cast<DWORD>(std::min(capacity, static_cast<std::size_t>(available))), &received, nullptr)) bytes = received;
        if (WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0) exited = true;
#else
        const auto received = read(pipes[0], buffer, capacity);
        if (received > 0) bytes = static_cast<std::size_t>(received);
        else if (received == 0 || (errno != EAGAIN && errno != EINTR)) eof = true;
        siginfo_t information{};
        if (!exited && waitid(P_PID, static_cast<id_t>(pid), &information, WEXITED | WNOHANG | WNOWAIT) == 0 && information.si_pid == pid) exited = true;
#endif
        result.byte_limit = bytes > max_bytes - result.output.size();
        result.output.append(buffer, std::min(bytes, max_bytes - result.output.size()));
        if (exited && eof) break;
        if (!bytes) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
#ifdef _WIN32
    if (!exited) {
        TerminateProcess(process.hProcess, 1);
        exited = WaitForSingleObject(process.hProcess, 100) == WAIT_OBJECT_0;
    }
    DWORD code = 0;
    if (exited && GetExitCodeProcess(process.hProcess, &code)) result.exit_code = static_cast<int>(code);
    CloseHandle(reader);
    CloseHandle(process.hProcess);
    CloseHandle(job);
#else
    if (!exited || !eof) ::kill(-pid, SIGKILL);
    const auto cleanup = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    bool reaped = false;
    while (!reaped && std::chrono::steady_clock::now() < cleanup) {
        const auto waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid) {
            reaped = true;
            if (WIFEXITED(status)) result.exit_code = WEXITSTATUS(status);
        } else if (waited < 0 && errno == ECHILD) reaped = true;
        else std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    close(pipes[0]);
#endif
    if (cancellation_error) std::rethrow_exception(cancellation_error);
    return result;
}

int ProcessManager::run_process_with_output(
    const std::string& executable,
    const std::vector<std::string>& args,
    OutputLineCallback on_line,
    const std::string& working_dir,
    int timeout_seconds,
    bool capture_stderr) {

    auto platform = create_process_platform();
    return platform->run_with_output(executable, args, on_line, working_dir, timeout_seconds, capture_stderr);
}

void ProcessManager::kill_process(ProcessHandle handle) {
    auto platform = create_process_platform();
    platform->kill(handle);
}

void ProcessManager::terminate_process(ProcessHandle handle) {
    auto platform = create_process_platform();
    platform->terminate_without_cleanup(handle);
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
