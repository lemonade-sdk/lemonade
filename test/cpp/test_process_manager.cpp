// Standalone test for ProcessManager::is_running() non-mutating contract on POSIX.
//
// Verifies:
//  1. is_running() returns true for a running process
//  2. is_running() returns false for an exited process WITHOUT reaping it
//  3. reap_process() retrieves the real exit code after is_running() returned false
//
// Compile:
//   g++ -std=c++17 -I src/cpp/include test/cpp/test_process_manager.cpp \
//       src/cpp/server/utils/process_manager.cpp \
//       src/cpp/server/utils/platform/process_unix.cpp \
//       -pthread -o test_process_manager
//
// Run:
//   ./test_process_manager

#include <lemon/utils/process_manager.h>
#include <lemon/utils/process_platform.h>

#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <thread>
#include <chrono>

#ifdef __linux__
#include <fstream>
#include <sstream>
#endif

using lemon::utils::ProcessHandle;
using lemon::utils::ProcessManager;

static int g_failures = 0;

static void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failures;
}

// Spawn a child via fork() that exits with a known code.
// Returns the child PID (or -1 on failure).
static pid_t spawn_child(int exit_code) {
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        _exit(exit_code);
    }
    return pid;
}

// Create a ProcessHandle from a raw PID.
static ProcessHandle make_handle(pid_t pid) {
    ProcessHandle h;
    h.handle = nullptr;
    h.pid = static_cast<int>(pid);
    return h;
}

// Wait for a child to exit without reaping it.
// Uses waitid(WNOWAIT) on platforms that support it, or /proc/<pid>/stat on Linux.
// Returns the child PID when it has exited (is a zombie), or -1 on timeout.
static pid_t wait_for_exit_no_reap(pid_t child, int timeout_ms = 5000) {
    auto start = std::chrono::steady_clock::now();
    while (true) {
#ifdef WNOWAIT
        siginfo_t info;
        std::memset(&info, 0, sizeof(info));
        if (waitid(P_PID, static_cast<id_t>(child), &info, WEXITED | WNOHANG | WNOWAIT) == 0) {
            if (info.si_pid != 0) {
                return child;
            }
        }
#endif
#ifdef __linux__
        {
            char path[64];
            std::snprintf(path, sizeof(path), "/proc/%d/stat", child);
            std::ifstream f(path);
            if (f.is_open()) {
                std::string line;
                if (std::getline(f, line)) {
                    auto open_paren = line.find('(');
                    if (open_paren != std::string::npos) {
                        auto close_paren = line.rfind(')');
                        if (close_paren != std::string::npos && close_paren > open_paren) {
                            std::string rest = line.substr(close_paren + 1);
                            std::istringstream iss(rest);
                            char state;
                            if (iss >> state && state == 'Z') {
                                return child;
                            }
                        }
                    }
                }
            }
        }
#endif
        // Fallback: try waitpid with WNOHANG. If the child has already
        // been reaped by something else (or we lost the race), return -1
        // to indicate we couldn't verify the exit without consuming it.
        int status = 0;
        pid_t r = waitpid(child, &status, WNOHANG);
        if (r == child) {
            return -1;
        }
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count() > timeout_ms) {
            return -1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

int main() {
    // --- Test: is_running() returns false for an exited child ---
    {
        pid_t child = spawn_child(42);
        check("spawn_child returns valid PID", child > 0);

        // Wait for the child to exit without reaping it.
        pid_t exited = wait_for_exit_no_reap(child);
        check("child exited (detected without reap)", exited > 0);

        if (exited > 0) {
            ProcessHandle h = make_handle(exited);

            // is_running() must NOT reap the child — it should just report
            // that the process is no longer running.
            bool running = ProcessManager::is_running(h);
            check("is_running() returns false for exited child", !running);

            // reap_process() should retrieve the real exit code (42).
            int exit_code = ProcessManager::reap_process(h);
            check("reap_process() returns real exit code 42", exit_code == 42);
        } else {
            // wait_for_exit_no_reap timed out — child already exited, so it's
            // a zombie. Clean it up to avoid leaking zombies.
            int status = 0;
            waitpid(child, &status, 0);
        }
    }

    // --- Test: is_running() returns false for PID 0 ---
    {
        ProcessHandle h = make_handle(0);
        check("is_running() returns false for PID 0", !ProcessManager::is_running(h));
    }

    // --- Test: is_running() returns false for negative PID ---
    {
        ProcessHandle h = make_handle(-1);
        check("is_running() returns false for negative PID", !ProcessManager::is_running(h));
    }

    // --- Test: is_running() returns false for a non-existent PID ---
    {
        // Use a PID that almost certainly doesn't exist.
        ProcessHandle h = make_handle(999999);
        check("is_running() returns false for non-existent PID", !ProcessManager::is_running(h));
    }

    // --- Test: is_running() returns true for a running process ---
    {
        pid_t child = fork();
        check("fork succeeds", child >= 0);

        if (child > 0) {
            ProcessHandle h = make_handle(child);
            check("is_running() returns true for running child", ProcessManager::is_running(h));

            // Clean up: kill the child and reap.
            kill(child, SIGKILL);
            int status = 0;
            waitpid(child, &status, 0);
        } else if (child == 0) {
            // Parent will SIGKILL us — just sleep forever.
            while (true) {
                sleep(1);
            }
        }
    }

    // --- Test: reap_process() returns -1 for a still-running process ---
    {
        pid_t child = fork();
        check("fork for reap test", child >= 0);

        if (child > 0) {
            ProcessHandle h = make_handle(child);
            // Give the child a moment to start.
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

            int rc = ProcessManager::reap_process(h);
            check("reap_process() returns -1 for running process", rc == -1);

            // Clean up.
            kill(child, SIGKILL);
            int status = 0;
            waitpid(child, &status, 0);
        } else if (child == 0) {
            while (true) {
                sleep(1);
            }
        }
    }

    if (g_failures == 0) {
        std::printf("\nAll process_manager tests passed\n");
        return 0;
    }
    std::printf("\n%d process_manager test(s) FAILED\n", g_failures);
    return 1;
}
