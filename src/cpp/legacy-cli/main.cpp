// Legacy shim: backwards-compatible lemonade-server binary
// Prints a deprecation notice and delegates to lemond or lemonade CLI.

#include <lemon/version.h>

#include <httplib.h>
#include <nlohmann/json.hpp>

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
using json = nlohmann::json;

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

/// Map old CLI args to config.json nested JSON.
/// Returns a pair: (lemond_args, config_updates)
static std::pair<std::vector<std::string>, json>
parse_serve_args(const std::vector<std::string>& args) {
    std::vector<std::string> lemond_args;
    json updates = json::object();

    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];
        auto next = [&]() -> std::string {
            if (i + 1 < args.size()) return args[++i];
            return "";
        };

        // Args that pass through to lemond
        if (arg == "--port") { lemond_args.push_back(arg); lemond_args.push_back(next()); }
        else if (arg == "--host") { lemond_args.push_back(arg); lemond_args.push_back(next()); }
        // Args that become /internal/set calls
        else if (arg == "--log-level") { updates["log_level"] = next(); }
        else if (arg == "--extra-models-dir") { updates["extra_models_dir"] = next(); }
        else if (arg == "--no-broadcast") { updates["no_broadcast"] = true; }
        else if (arg == "--global-timeout") { updates["global_timeout"] = std::stoi(next()); }
        else if (arg == "--max-loaded-models") { updates["max_loaded_models"] = std::stoi(next()); }
        else if (arg == "--ctx-size") { updates["ctx_size"] = std::stoi(next()); }
        else if (arg == "--llamacpp") {
            if (!updates.contains("llamacpp")) updates["llamacpp"] = json::object();
            updates["llamacpp"]["backend"] = next();
        }
        else if (arg == "--llamacpp-args") {
            if (!updates.contains("llamacpp")) updates["llamacpp"] = json::object();
            updates["llamacpp"]["args"] = next();
        }
        else if (arg == "--whispercpp") {
            if (!updates.contains("whispercpp")) updates["whispercpp"] = json::object();
            updates["whispercpp"]["backend"] = next();
        }
        else if (arg == "--whispercpp-args") {
            if (!updates.contains("whispercpp")) updates["whispercpp"] = json::object();
            updates["whispercpp"]["args"] = next();
        }
        else if (arg == "--sdcpp") {
            if (!updates.contains("sdcpp")) updates["sdcpp"] = json::object();
            updates["sdcpp"]["backend"] = next();
        }
        else if (arg == "--steps") {
            if (!updates.contains("sdcpp")) updates["sdcpp"] = json::object();
            updates["sdcpp"]["steps"] = std::stoi(next());
        }
        else if (arg == "--cfg-scale") {
            if (!updates.contains("sdcpp")) updates["sdcpp"] = json::object();
            updates["sdcpp"]["cfg_scale"] = std::stod(next());
        }
        else if (arg == "--width") {
            if (!updates.contains("sdcpp")) updates["sdcpp"] = json::object();
            updates["sdcpp"]["width"] = std::stoi(next());
        }
        else if (arg == "--height") {
            if (!updates.contains("sdcpp")) updates["sdcpp"] = json::object();
            updates["sdcpp"]["height"] = std::stoi(next());
        }
        else if (arg == "--flm-args") {
            if (!updates.contains("flm")) updates["flm"] = json::object();
            updates["flm"]["args"] = next();
        }
        else if (arg == "--no-tray") {
            // Ignored (no longer applicable)
        }
        else {
            // Unknown arg — pass through (might be home_dir positional)
            lemond_args.push_back(arg);
        }
    }

    return {lemond_args, updates};
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

/// Apply config updates via /internal/set
static bool apply_config(const std::string& host, int port, const json& updates) {
    httplib::Client client(host, port);
    client.set_connection_timeout(5);
    client.set_read_timeout(5);

    auto res = client.Post("/internal/set", updates.dump(), "application/json");
    if (res && res->status >= 200 && res->status < 300) {
        return true;
    }

    std::cerr << "error: failed to apply configuration via /internal/set";
    if (res) {
        std::cerr << " (HTTP " << res->status << ")";
    }
    std::cerr << std::endl;
    return false;
}

/// Serve command: spawn lemond, wait for health, apply config updates, then wait.
static int do_serve(const fs::path& dir, const std::vector<std::string>& args) {
    auto [lemond_args, config_updates] = parse_serve_args(args);

    // If there are no config updates, just exec lemond directly (simpler)
    if (config_updates.empty()) {
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
    int port = 8000;
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

    // Apply config updates
    if (!config_updates.empty()) {
        apply_config(health_host, port, config_updates);
    }

    // Wait for child to exit (forward signals)
    int status;
    waitpid(child, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;

#else
    // Windows: spawn lemond as a child process
    std::string cmd_line = server.string();
    for (const auto& a : lemond_args) {
        cmd_line += " " + a;
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

    // Apply config updates
    if (!config_updates.empty()) {
        apply_config(health_host, port, config_updates);
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
    if (port == 0) port = 8000;

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
