// Legacy shim: backwards-compatible lemonade-server binary
// Prints a deprecation notice and delegates to lemond or lemonade CLI.

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
    // Fallback: derive from argv[0] would require passing it in, but
    // on Unix /proc/self/exe is reliable.
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

/// Discover the active server port by running `lemonade status --json`.
/// Returns 0 on failure (caller should fall back to default).
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

    // Parse {"port": N}
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
    // Discover port: try lemonade status --json, fall back to 8000
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
        fs::path server = sibling_exe(dir, "lemond");
#endif
        // Strip flags that were valid for old lemonade-server but not for lemond
        std::vector<std::string> server_args;
        for (size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--no-tray") continue;
            server_args.push_back(args[i]);
        }
        exec_program(server, server_args);
        // exec_program is [[noreturn]]
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
