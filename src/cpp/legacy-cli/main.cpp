// Legacy shim: backwards-compatible lemonade-server binary
// Prints a deprecation notice and delegates to lemonade-router or lemonade CLI.

#include <lemon/version.h>

#include <httplib.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <process.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void print_deprecation_notice() {
    std::cerr
        << "WARNING: 'lemonade-server' is deprecated. Use 'lemonade-router' to start the server,\n"
        << "or 'lemonade' for CLI commands. See 'lemonade --help' for details.\n"
        << std::endl;
}

static void print_help() {
    std::cout
        << "lemonade-server " << LEMON_VERSION_STRING << " (deprecated shim)\n"
        << "\n"
        << "This binary is a backwards-compatibility shim. All functionality has moved:\n"
        << "\n"
        << "  OLD COMMAND                    NEW COMMAND\n"
        << "  -----------------------------------------------------------\n"
        << "  lemonade-server serve [args]   lemonade-router [args]\n"
        << "  lemonade-server stop           lemonade stop\n"
        << "  lemonade-server list           lemonade list\n"
        << "  lemonade-server pull <model>   lemonade pull <model>\n"
        << "  lemonade-server delete <model> lemonade delete <model>\n"
        << "  lemonade-server run <model>    lemonade run <model>\n"
        << "  lemonade-server status         lemonade status\n"
        << "  lemonade-server logs           lemonade logs\n"
        << "\n"
        << "Use 'lemonade-router' to start the server, or 'lemonade' for\n"
        << "all other CLI commands.\n";
}

static void print_version() {
    std::cout << "lemonade-server " << LEMON_VERSION_STRING << " (deprecated shim)" << std::endl;
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
    // Fallback: derive from argv[0] would require passing it in, but
    // on Unix /proc/self/exe is reliable.
#ifdef __linux__
    std::error_code ec;
    auto p = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) return p.parent_path();
#endif
#ifdef __APPLE__
    // _NSGetExecutablePath could be used; keep it simple with argv[0].
#endif
    return fs::path(); // empty — will search PATH as fallback
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
/// On Windows uses _spawnvp with _P_OVERLAY (equivalent to exec).
/// On Unix uses execvp.
[[noreturn]]
static void exec_program(const fs::path &exe_path, const std::vector<std::string> &args) {
    // Build a C-style argv array
    std::vector<const char *> argv;
    std::string exe_str = exe_path.string();
    argv.push_back(exe_str.c_str());
    for (const auto &a : args) {
        argv.push_back(a.c_str());
    }
    argv.push_back(nullptr);

#ifdef _WIN32
    // _spawnvp with _P_OVERLAY replaces the current process image
    intptr_t rc = _spawnvp(_P_OVERLAY,
                           exe_str.c_str(),
                           const_cast<char *const *>(argv.data()));
    // If _spawnvp returns, it failed
    std::cerr << "error: failed to exec '" << exe_str << "': " << strerror(errno) << std::endl;
    _exit(static_cast<int>(rc));
#else
    execvp(exe_str.c_str(), const_cast<char *const *>(argv.data()));
    // If execvp returns, it failed
    std::cerr << "error: failed to exec '" << exe_str << "': " << strerror(errno) << std::endl;
    _exit(127);
#endif
}

// ---------------------------------------------------------------------------
// Stop command — POST /internal/shutdown to the running server
// ---------------------------------------------------------------------------

/// Parse --port from the argument list; returns default_port if not found.
static int find_port(const std::vector<std::string> &args, int default_port) {
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--port" && i + 1 < args.size()) {
            try {
                return std::stoi(args[i + 1]);
            } catch (...) {
                // ignore parse error, fall back to default
            }
        }
    }
    return default_port;
}

static int do_stop(const std::vector<std::string> &args) {
    int port = find_port(args, 8000);

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

    // Collect args after argv[0]
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }

    // No arguments — print help
    if (args.empty()) {
        print_help();
        return 0;
    }

    const std::string &cmd = args[0];

    // --help / -h
    if (cmd == "--help" || cmd == "-h") {
        print_help();
        return 0;
    }

    // --version / -v
    if (cmd == "--version" || cmd == "-v") {
        print_version();
        return 0;
    }

    fs::path dir = get_exe_dir();

    // serve → delegate to appropriate server binary
    if (cmd == "serve") {
#ifdef _WIN32
        // On Windows, LemonadeServer.exe is the embedded server + tray
        fs::path server = sibling_exe(dir, "LemonadeServer");
#else
        fs::path server = sibling_exe(dir, "lemonade-router");
#endif
        std::vector<std::string> server_args(args.begin() + 1, args.end());
        exec_program(server, server_args);
        // exec_program is [[noreturn]]
    }

    // stop → HTTP POST to /internal/shutdown
    if (cmd == "stop") {
        std::vector<std::string> stop_args(args.begin() + 1, args.end());
        return do_stop(stop_args);
    }

    // Everything else → delegate to lemonade CLI
    fs::path cli = sibling_exe(dir, "lemonade");
    exec_program(cli, args);
    // exec_program is [[noreturn]]
}
