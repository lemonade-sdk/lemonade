// Legacy shim: backwards-compatible lemonade-server binary
// Prints a deprecation notice and delegates to lemond or lemonade CLI.

#include <lemon/version.h>

#include <httplib.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <process.h>
#else
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static const char* server_binary_name() {
#ifdef _WIN32
    return "LemonadeServer.exe";
#else
    return "lemond";
#endif
}

static void print_deprecation_notice() {
    std::cerr
        << "WARNING: 'lemonade-server' is deprecated. Use '" << server_binary_name()
        << "' to start the server,\nor 'lemonade' for CLI commands. See 'lemonade --help' for details.\n"
        << std::endl;
}

static void print_help() {
    const char* srv = server_binary_name();
    std::cout
        << "lemonade-server " << LEMON_VERSION_STRING << " (deprecated shim)\n"
        << "\n"
        << "This binary is a backwards-compatibility shim. All functionality has moved:\n"
        << "\n"
        << "  OLD COMMAND                    NEW COMMAND\n"
        << "  -----------------------------------------------------------\n"
        << "  lemonade-server serve [args]   " << srv << " [args]\n"
        << "  lemonade-server stop           lemonade stop\n"
        << "  lemonade-server list           lemonade list\n"
        << "  lemonade-server pull <model>   lemonade pull <model>\n"
        << "  lemonade-server delete <model> lemonade delete <model>\n"
        << "  lemonade-server run <model>    lemonade run <model>\n"
        << "  lemonade-server status         lemonade status\n"
        << "  lemonade-server logs           lemonade logs\n"
        << "\n"
        << "Use '" << srv << "' to start the server, or 'lemonade' for\n"
        << "all other CLI commands.\n";
}

static void print_version() {
    std::cout << "lemonade-server version " << LEMON_VERSION_STRING << std::endl;
}

/// Return the directory that contains this executable.
static fs::path get_exe_dir() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        return fs::path(buf).parent_path();
    }
#endif
#ifdef __linux__
    std::error_code ec;
    auto p = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) return p.parent_path();
#endif
#ifdef __APPLE__
    char buf[1024];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) {
        std::error_code ec;
        auto real = fs::canonical(buf, ec);
        if (!ec) return real.parent_path();
        return fs::path(buf).parent_path();
    }
#endif
    return fs::path();
}

/// Build the full path to a sibling executable, adding .exe on Windows.
static fs::path sibling_exe(const fs::path &dir, const std::string &name) {
    fs::path p = dir / name;
#ifdef _WIN32
    p.replace_extension(".exe");
#endif
    return p;
}

// ---------------------------------------------------------------------------
// Exec helpers
// ---------------------------------------------------------------------------

/// Replace this process with the given executable and arguments.
[[noreturn]]
static void exec_program(const fs::path &exe_path, const std::vector<std::string> &args) {
    std::vector<const char *> argv;
    std::string exe_str = exe_path.string();
    argv.push_back(exe_str.c_str());
    for (const auto &a : args) {
        argv.push_back(a.c_str());
    }
    argv.push_back(nullptr);

#ifdef _WIN32
    intptr_t rc = _spawnvp(_P_OVERLAY,
                           exe_str.c_str(),
                           const_cast<char *const *>(argv.data()));
    std::cerr << "error: failed to exec '" << exe_str << "': " << strerror(errno) << std::endl;
    _exit(static_cast<int>(rc));
#else
    execvp(exe_str.c_str(), const_cast<char *const *>(argv.data()));
    std::cerr << "error: failed to exec '" << exe_str << "': " << strerror(errno) << std::endl;
    _exit(127);
#endif
}

// ---------------------------------------------------------------------------
// Serve command: parse old args, spawn lemond, configure via /internal/set
// ---------------------------------------------------------------------------

/// Map old CLI args to lemond args and lemonade config set args.
/// Returns a pair: (lemond_args, config_set_args)
/// config_set_args use key=value format for `lemonade config set`, e.g.:
///   {"log_level=debug", "llamacpp.backend=vulkan"}
static std::pair<std::vector<std::string>, std::vector<std::string>>
parse_serve_args(const std::vector<std::string>& args) {
    std::vector<std::string> lemond_args;
    std::vector<std::string> config_set_args;

    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];
        auto next = [&]() -> std::string {
            if (i + 1 < args.size()) return args[++i];
            return "";
        };

        // Args that pass through to lemond
        if (arg == "--port") { lemond_args.push_back(arg); lemond_args.push_back(next()); }
        else if (arg == "--host") { lemond_args.push_back(arg); lemond_args.push_back(next()); }
        // Args that become `lemonade config set key=value` pairs
        else if (arg == "--log-level") { config_set_args.push_back("log_level=" + next()); }
        else if (arg == "--extra-models-dir") { config_set_args.push_back("extra_models_dir=" + next()); }
        else if (arg == "--no-broadcast") { config_set_args.push_back("no_broadcast=true"); }
        else if (arg == "--global-timeout") { config_set_args.push_back("global_timeout=" + next()); }
        else if (arg == "--max-loaded-models") { config_set_args.push_back("max_loaded_models=" + next()); }
        else if (arg == "--ctx-size") { config_set_args.push_back("ctx_size=" + next()); }
        else if (arg == "--llamacpp") { config_set_args.push_back("llamacpp.backend=" + next()); }
        else if (arg == "--llamacpp-args") { config_set_args.push_back("llamacpp.args=" + next()); }
        else if (arg == "--whispercpp") { config_set_args.push_back("whispercpp.backend=" + next()); }
        else if (arg == "--whispercpp-args") { config_set_args.push_back("whispercpp.args=" + next()); }
        else if (arg == "--sdcpp") { config_set_args.push_back("sdcpp.backend=" + next()); }
        else if (arg == "--sdcpp-args") { config_set_args.push_back("sdcpp.args=" + next()); }
        else if (arg == "--steps") { config_set_args.push_back("sdcpp.steps=" + next()); }
        else if (arg == "--cfg-scale") { config_set_args.push_back("sdcpp.cfg_scale=" + next()); }
        else if (arg == "--width") { config_set_args.push_back("sdcpp.width=" + next()); }
        else if (arg == "--height") { config_set_args.push_back("sdcpp.height=" + next()); }
        else if (arg == "--flm-args") { config_set_args.push_back("flm.args=" + next()); }
        else if (arg == "--no-tray") {
            // Ignored (no longer applicable)
        }
        else {
            // Unknown arg — pass through (might be cache_dir positional)
            lemond_args.push_back(arg);
        }
    }

    return {lemond_args, config_set_args};
}

/// Wait for lemond to become healthy, up to timeout_seconds.
static bool wait_for_health(const std::string& host, int port, int timeout_seconds) {
    httplib::Client client(host, port);
    client.set_connection_timeout(2);
    client.set_read_timeout(2);

    auto start = std::chrono::steady_clock::now();
    while (true) {
        auto res = client.Get("/api/v1/health");
        if (res && res->status == 200) {
            return true;
        }

        auto elapsed = std::chrono::steady_clock::now() - start;
        if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() >= timeout_seconds) {
            return false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

/// Apply config updates via `lemonade config set`
static bool apply_config(const fs::path& dir, const std::string& host, int port,
                         const std::vector<std::string>& config_set_args) {
    fs::path cli = sibling_exe(dir, "lemonade");
    std::vector<std::string> full_args;
    full_args.push_back("--host");
    full_args.push_back(host);
    full_args.push_back("--port");
    full_args.push_back(std::to_string(port));
    full_args.push_back("config");
    full_args.push_back("set");
    full_args.insert(full_args.end(), config_set_args.begin(), config_set_args.end());

    // Build command line and run as a child process
    std::vector<const char*> argv;
    std::string exe_str = cli.string();
    argv.push_back(exe_str.c_str());
    for (const auto& a : full_args) {
        argv.push_back(a.c_str());
    }
    argv.push_back(nullptr);

#ifdef _WIN32
    intptr_t rc = _spawnvp(_P_WAIT, exe_str.c_str(),
                           const_cast<char* const*>(argv.data()));
    return rc == 0;
#else
    pid_t pid = fork();
    if (pid < 0) {
        std::cerr << "error: fork failed: " << strerror(errno) << std::endl;
        return false;
    }
    if (pid == 0) {
        execvp(exe_str.c_str(), const_cast<char* const*>(argv.data()));
        std::cerr << "error: failed to exec '" << exe_str << "': " << strerror(errno) << std::endl;
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
}

/// Serve command: spawn lemond, wait for health, apply config updates, then wait.
static int do_serve(const fs::path& dir, const std::vector<std::string>& args) {
    auto [lemond_args, config_set_args] = parse_serve_args(args);

    // If there are no config updates, just exec lemond directly (simpler)
    if (config_set_args.empty()) {
#ifdef _WIN32
        fs::path server = sibling_exe(dir, "LemonadeServer");
#else
        fs::path server = sibling_exe(dir, "lemond");
#endif
        exec_program(server, lemond_args);
        // exec_program is [[noreturn]]
    }

    // Need to spawn lemond as a child process so we can apply config after it starts
    fs::path server = sibling_exe(dir, "lemond");

    // Extract port/host from lemond_args for health check
    int port = 13305;
    std::string host = "127.0.0.1";
    for (size_t i = 0; i < lemond_args.size(); ++i) {
        if (lemond_args[i] == "--port" && i + 1 < lemond_args.size()) {
            port = std::stoi(lemond_args[i + 1]);
        }
        if (lemond_args[i] == "--host" && i + 1 < lemond_args.size()) {
            host = lemond_args[i + 1];
        }
    }
    // For health check, always use localhost
    std::string health_host = "127.0.0.1";

#ifndef _WIN32
    // Fork and exec lemond
    pid_t child = fork();
    if (child < 0) {
        std::cerr << "error: fork failed: " << strerror(errno) << std::endl;
        return 1;
    }

    if (child == 0) {
        // Child process: exec lemond
        std::vector<const char*> argv;
        std::string exe_str = server.string();
        argv.push_back(exe_str.c_str());
        for (const auto& a : lemond_args) {
            argv.push_back(a.c_str());
        }
        argv.push_back(nullptr);
        execvp(exe_str.c_str(), const_cast<char *const *>(argv.data()));
        std::cerr << "error: failed to exec '" << exe_str << "': " << strerror(errno) << std::endl;
        _exit(127);
    }

    // Parent: wait for lemond to become healthy
    if (!wait_for_health(health_host, port, 30)) {
        std::cerr << "error: lemond did not become healthy within 30 seconds" << std::endl;
        kill(child, SIGTERM);
        waitpid(child, nullptr, 0);
        return 1;
    }

    // Apply config updates via lemonade config set
    if (!config_set_args.empty()) {
        apply_config(dir, health_host, port, config_set_args);
    }

    // Wait for child to exit (forward signals)
    int status;
    waitpid(child, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;

#else
    // Windows: spawn lemond as a child process
    std::string cmd_line = "\"" + server.string() + "\"";
    for (const auto& a : lemond_args) {
        cmd_line += " \"" + a + "\"";
    }

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    if (!CreateProcessA(nullptr, const_cast<char*>(cmd_line.c_str()),
                        nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        std::cerr << "error: failed to start lemond" << std::endl;
        return 1;
    }

    // Wait for lemond to become healthy
    if (!wait_for_health(health_host, port, 30)) {
        std::cerr << "error: lemond did not become healthy within 30 seconds" << std::endl;
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return 1;
    }

    // Apply config updates via lemonade config set
    if (!config_set_args.empty()) {
        apply_config(dir, health_host, port, config_set_args);
    }

    // Wait for child to exit
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<int>(exit_code);
#endif
}

// ---------------------------------------------------------------------------
// Stop command — POST /internal/shutdown to the running server
// ---------------------------------------------------------------------------

static int discover_port(const fs::path &dir) {
    fs::path cli = sibling_exe(dir, "lemonade");
    std::string cmd = cli.string() + " status --json";

#ifdef _WIN32
    FILE* pipe = _popen(cmd.c_str(), "r");
#else
    FILE* pipe = popen(cmd.c_str(), "r");
#endif
    if (!pipe) return 0;

    char buf[256];
    std::string output;
    while (fgets(buf, sizeof(buf), pipe)) {
        output += buf;
    }

#ifdef _WIN32
    int status = _pclose(pipe);
#else
    int status = pclose(pipe);
#endif
    if (status != 0) return 0;

    try {
        auto pos = output.find("\"port\"");
        if (pos == std::string::npos) return 0;
        pos = output.find(':', pos);
        if (pos == std::string::npos) return 0;
        return std::stoi(output.substr(pos + 1));
    } catch (...) {
        return 0;
    }
}

static int do_stop(const fs::path &dir) {
    int port = discover_port(dir);
    if (port == 0) port = 13305;

    httplib::Client client("127.0.0.1", port);
    client.set_connection_timeout(5);
    client.set_read_timeout(5);

    auto res = client.Post("/internal/shutdown", "", "application/json");
    if (res) {
        if (res->status >= 200 && res->status < 300) {
            std::cout << "Server on port " << port << " has been asked to shut down." << std::endl;
            return 0;
        } else {
            std::cerr << "error: server returned HTTP " << res->status << std::endl;
            return 1;
        }
    } else {
        std::cerr << "error: could not connect to server on port " << port
                  << " (" << httplib::to_string(res.error()) << ")" << std::endl;
        return 1;
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char *argv[]) {
    print_deprecation_notice();

    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }

    if (args.empty()) {
        print_help();
        return 0;
    }

    const std::string &cmd = args[0];

    if (cmd == "--help" || cmd == "-h") {
        print_help();
        return 0;
    }

    if (cmd == "--version" || cmd == "-v") {
        print_version();
        return 0;
    }

    fs::path dir = get_exe_dir();

    // serve → spawn lemond, optionally configure via /internal/set
    if (cmd == "serve") {
        return do_serve(dir, args);
    }

    // stop → discover port via lemonade status --json, then POST /internal/shutdown
    if (cmd == "stop") {
        return do_stop(dir);
    }

    // Everything else → delegate to lemonade CLI
    fs::path cli = sibling_exe(dir, "lemonade");
    exec_program(cli, args);
    // exec_program is [[noreturn]]
}
