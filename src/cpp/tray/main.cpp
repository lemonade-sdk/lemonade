// Tray application entry point
//
// Windows (SUBSYSTEM:WINDOWS):
//   Embeds lemon::Server on a background thread, then runs TrayUI.
//   Output binary: LemonadeServer.exe
//
// macOS / Linux:
//   Connects to an already-running lemond, then runs TrayUI.
//   Output binary: lemonade-tray

#include "lemon_tray/tray_ui.h"
#include <lemon/single_instance.h>
#include <lemon/utils/aixlog.hpp>
#include <lemon/utils/url_utils.h>
#include <lemon/version.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <CLI/CLI.hpp>
#include <httplib.h>

#ifdef _WIN32
// Windows embeds the server
#include <lemon/cli_parser.h>
#include <lemon/config_file.h>
#include <lemon/logging_config.h>
#include <lemon/runtime_config.h>
#include <lemon/server.h>
#include <lemon/utils/json_utils.h>
#include <lemon/utils/path_utils.h>
#include <winsock2.h>
#include <windows.h>

// ---------------------------------------------------------------------------
// Windows Job Object — ensures child processes (llama-server, etc.) are
// automatically killed when LemonadeServer.exe exits for ANY reason
// (graceful quit, crash, taskkill, installer uninstall).
// ---------------------------------------------------------------------------
static HANDLE g_job_object = nullptr;

static void create_child_process_job() {
    g_job_object = CreateJobObjectA(nullptr, nullptr);
    if (!g_job_object) return;

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {};
    jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(g_job_object,
                            JobObjectExtendedLimitInformation,
                            &jeli, sizeof(jeli));

    // Assign current process to the job.  All child processes created via
    // CreateProcess will inherit the job (unless CREATE_BREAKAWAY_FROM_JOB
    // is used, which our ProcessManager does not).  When the last handle to
    // the job is closed (i.e. when this process exits), Windows terminates
    // every remaining process in the job.
    AssignProcessToJobObject(g_job_object, GetCurrentProcess());
}
#else
#include <csignal>
#include <unistd.h>
#endif

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

enum class ServerProbeMode {
    ready_only,     // 200, 401, 403 (503 retries until ready or timeout)
    allow_starting  // 200, 401, 403, 503 (returns true immediately if starting or ready)
};

static bool wait_for_server(const std::string& clean_host, int clean_port, bool is_ssl, int timeout_seconds, ServerProbeMode mode = ServerProbeMode::ready_only) {
    std::string connect_host;
    if (is_ssl) {
        connect_host = (clean_host.empty() || clean_host == "0.0.0.0")
            ? "127.0.0.1" : clean_host;
    } else {
        connect_host = (clean_host.empty() || clean_host == "0.0.0.0" || clean_host == "localhost")
            ? "127.0.0.1" : clean_host;
    }

    // Pass API key if set - prefer admin key over regular API key
    const char* admin_api_key = std::getenv("LEMONADE_ADMIN_API_KEY");
    const char* api_key = admin_api_key ? admin_api_key : std::getenv("LEMONADE_API_KEY");
    httplib::Headers headers;
    if (api_key && api_key[0]) {
        headers.emplace("Authorization", std::string("Bearer ") + api_key);
    }

    int max_attempts = std::max(1, timeout_seconds * 2);
    for (int i = 0; i < max_attempts; ++i) {
        try {
#ifndef LEMONADE_HTTPLIB_HAS_TLS
            if (is_ssl) {
                std::cerr << "HTTPS support is not compiled in this client." << std::endl;
                return false;
            }
#endif
            std::string format_host = lemon::utils::bracket_host_if_ipv6(connect_host);
            std::string scheme = is_ssl ? "https" : "http";
            std::string url = scheme + "://" + format_host + ":" + std::to_string(clean_port);
            httplib::Client cli(url);
#ifdef LEMONADE_HTTPLIB_HAS_TLS
            const char* skip_verify = std::getenv("LEMONADE_SKIP_VERIFY");
            if (skip_verify && std::string(skip_verify) == "1") {
                cli.enable_server_certificate_verification(false);
            }
#endif
            cli.set_connection_timeout(1);
            cli.set_read_timeout(5);
            // Use /api/v1/health instead of /live — /live responds before the model
            // cache is built, which causes 500s on /models if clients connect too early.
            auto res = cli.Get("/api/v1/health", headers);
            if (res) {
                if (res->status == 200 || res->status == 401 || res->status == 403) {
                    return true;
                }
                if (res->status == 503 && mode == ServerProbeMode::allow_starting) {
                    return true;
                }
            }
        } catch (...) {}
        if (i + 1 < max_attempts) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Windows entry point (SUBSYSTEM:WINDOWS — embedded server)
// ---------------------------------------------------------------------------

#ifdef _WIN32

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    // Create a job object so that all child processes (llama-server, etc.)
    // are automatically killed when this process exits.
    create_child_process_job();

    // Single instance check — prevents running alongside lemond
    if (lemon::SingleInstance::IsAnotherInstanceRunning("Router")) {
        return 0;
    }

    // Convert wide command line to argc/argv for CLI11
    int argc;
    LPWSTR* argvW = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> arg_strings(argc);
    std::vector<char*> argv_ptrs(argc);
    for (int i = 0; i < argc; ++i) {
        int len = WideCharToMultiByte(CP_UTF8, 0, argvW[i], -1, nullptr, 0, NULL, NULL);
        arg_strings[i].resize(len);
        WideCharToMultiByte(CP_UTF8, 0, argvW[i], -1, &arg_strings[i][0], len, NULL, NULL);
        if (!arg_strings[i].empty() && arg_strings[i].back() == '\0')
            arg_strings[i].pop_back();
        argv_ptrs[i] = &arg_strings[i][0];
    }
    LocalFree(argvW);

    // Attach to the parent's console (if launched from a terminal) so that
    // --help and --version print to the terminal the user typed in.
    // Fails silently when launched from Start Menu / shortcut (no parent console).
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* dummy;
        freopen_s(&dummy, "CONOUT$", "w", stdout);
        freopen_s(&dummy, "CONOUT$", "w", stderr);
    }

    // Parse CLI args. LemonadeServer.exe shares the server's CLIParser for
    // --port, --host, --log-level, etc.  We add --silent here (tray-only flag
    // used by the Windows startup shortcut to suppress the startup notification).
    bool silent = false;
    lemon::CLIParser parser;
    parser.add_flag("--silent", silent, "Suppress startup notification");
    parser.parse(argc, argv_ptrs.data());
    if (!parser.should_continue()) {
        return parser.get_exit_code();
    }
    auto cli_config = parser.get_config();

    lemon::utils::set_cache_dir(cli_config.cache_dir);
    lemon::utils::set_config_dir(cli_config.config_dir);
    lemon::utils::migrate_legacy_json_files_to_config_dir(cli_config.cache_dir,
                                                          cli_config.config_dir);

    auto config_json = lemon::ConfigFile::load(cli_config.cache_dir,
                                               cli_config.config_dir);

    auto runtime_config = std::make_shared<lemon::RuntimeConfig>(config_json);
    lemon::RuntimeConfig::set_global(runtime_config.get());

    if (cli_config.port != -1) {
        runtime_config->set_port_override(cli_config.port);
    }
    if (!cli_config.host.empty()) {
        runtime_config->set_host_override(cli_config.host);
    }
    if (!cli_config.log_file.empty()) {
        runtime_config->set_log_file_override(cli_config.log_file);
    }
    if (cli_config.log_max_file_size_mb != -1) {
        runtime_config->set_log_max_file_size_mb_override(cli_config.log_max_file_size_mb);
    }
    if (cli_config.log_max_files != -1) {
        runtime_config->set_log_max_files_override(cli_config.log_max_files);
    }

    lemon::utils::set_models_dir(runtime_config->models_dir());

    // Initialize logging (file + log hub; SUBSYSTEM:WINDOWS has no console)
    lemon::LogRotationConfig rot_cfg;
    rot_cfg.file_mode = runtime_config->log_file();
    rot_cfg.max_file_size_mb = runtime_config->log_max_file_size_mb();
    rot_cfg.max_files = runtime_config->log_max_files();
    lemon::configure_application_logging(
        runtime_config->log_level(), lemon::LoggingMode::embedded_tray_server, rot_cfg);

    // Initialize Winsock (required by httplib)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    // Start server on background thread
    std::string cache_dir = cli_config.cache_dir;
    std::string config_dir = cli_config.config_dir;
    std::thread server_thread([runtime_config, cache_dir, config_dir]() {
        try {
            lemon::Server server(runtime_config, cache_dir, config_dir);
            server.run();
        } catch (const std::exception& e) {
            MessageBoxA(NULL, e.what(), "Lemonade Server Error", MB_OK | MB_ICONERROR);
        }
    });
    server_thread.detach();

    // Wait for server to be ready
    if (!wait_for_server(runtime_config->host(), runtime_config->port(), false, 15)) {
        MessageBoxA(NULL,
            "Lemonade Server failed to start within 15 seconds.",
            "Lemonade Server Error", MB_OK | MB_ICONERROR);
        WSACleanup();
        return 1;
    }

    // Create and run tray UI.  If initialization fails (e.g. no display
    // server in CI, headless VM, or RDP session), fall back to running
    // headless — the server is already handling requests on the background
    // thread; we just need to block until shutdown.
    bool headless = false;
    try {
        lemon_tray::TrayUIOptions options;
        options.port = runtime_config->port();
        options.host = runtime_config->host();
        options.is_ssl = false;
        options.silent = silent;
        options.server_initially_connected = true;  // Embedded server is always up
        lemon_tray::TrayUI tray(options);
        if (tray.initialize()) {
            tray.run();  // Blocks until quit
        } else {
            LOG(WARNING, "Tray") << "Tray UI initialization failed — running headless" << std::endl;
            headless = true;
        }
    } catch (const std::exception& e) {
        LOG(WARNING, "Tray") << "Tray UI error: " << e.what() << " — running headless" << std::endl;
        headless = true;
    } catch (...) {
        LOG(WARNING, "Tray") << "Tray UI error — running headless" << std::endl;
        headless = true;
    }

    if (headless) {
        // Server is running on the background thread.
        // Block until /internal/shutdown calls std::exit(0).
        while (true) {
            std::this_thread::sleep_for(std::chrono::hours(24));
        }
    }

    // Shutdown the embedded server.
    // /internal/shutdown unloads all models synchronously (kills child
    // processes like llama-server) before sending the response, then
    // stops the HTTP listener and exits on a detached thread.
    {
        std::string connect_host = (runtime_config->host().empty() || runtime_config->host() == "0.0.0.0" || runtime_config->host() == "localhost")
            ? "127.0.0.1" : runtime_config->host();
        httplib::Client cli(connect_host, runtime_config->port());
        cli.set_connection_timeout(2);
        cli.set_read_timeout(30);  // Allow time for model unload (up to 5s per model)
        cli.Post("/internal/shutdown", "", "application/json");
    }

    // Give server a moment to stop the HTTP listener and exit
    std::this_thread::sleep_for(std::chrono::seconds(2));

    WSACleanup();
    return 0;
}

// ---------------------------------------------------------------------------
// macOS / Linux entry point (connects to running router)
// ---------------------------------------------------------------------------

#else

#include <CLI/CLI.hpp>
#include <lemon/utils/url_utils.h>
#include <lemon/utils/path_utils.h>
#include <lemon/single_instance.h>
#include <filesystem>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <vector>
#include <fstream>
#include <nlohmann/json.hpp>

// std::exit() is not async-signal-safe; set a flag the refresh thread polls.
static void tray_signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        lemon_tray::TrayUI::g_quit_requested = 1;
    }
}

static std::string find_lemond_binary(const char* argv0 = nullptr) {
    std::string bin_name = "lemond";
    if (argv0 && argv0[0]) {
        try {
            std::filesystem::path p(argv0);
            if (p.has_parent_path()) {
                std::filesystem::path candidate = p.parent_path() / bin_name;
                if (std::filesystem::exists(candidate) && std::filesystem::is_regular_file(candidate)) {
                    return candidate.string();
                }
            }
        } catch (...) {}
    }

    try {
        std::string exe_dir = lemon::utils::get_executable_dir();
        if (!exe_dir.empty()) {
            std::filesystem::path candidate = std::filesystem::path(exe_dir) / bin_name;
            if (std::filesystem::exists(candidate) && std::filesystem::is_regular_file(candidate)) {
                return candidate.string();
            }
        }
    } catch (...) {}

    std::string in_path = lemon::utils::find_executable_in_path(bin_name);
    if (!in_path.empty()) {
        return in_path;
    }

    std::vector<std::string> standard_paths = {
        "/usr/local/bin/lemond",
        "/usr/bin/lemond",
        "/opt/lemonade/bin/lemond"
    };
    const char* home = std::getenv("HOME");
    if (home && home[0]) {
        standard_paths.push_back(std::string(home) + "/.local/bin/lemond");
    }
    for (const auto& p : standard_paths) {
        if (std::filesystem::exists(p) && std::filesystem::is_regular_file(p)) {
            return p;
        }
    }
    return "";
}

static void read_fallback_config(int& port, std::string& host) {
    try {
        std::filesystem::path config_dir = lemon::utils::get_config_dir();
        if (!config_dir.empty()) {
            auto config_path = config_dir / "config.json";
            if (std::filesystem::exists(config_path)) {
                std::ifstream f(config_path);
                nlohmann::json j;
                f >> j;
                if (j.contains("port") && j["port"].is_number()) {
                    port = j["port"];
                }
                if (j.contains("host") && j["host"].is_string()) {
                    host = j["host"];
                }
            }
        }
    } catch (...) {}
}

extern char **environ;

int main(int argc, char* argv[]) {
    if (lemon::SingleInstance::IsAnotherInstanceRunning("Tray")) {
        std::cerr << "lemonade-tray is already running." << std::endl;
        return 0;
    }

    CLI::App app{"Lemonade Tray - system tray interface for Lemonade Server"};

    // config.json supplies the defaults that --port/--host override
    int port = 13305;
    std::string host = "localhost";
    read_fallback_config(port, host);

    app.add_option("--port,-p", port, "Server port to connect to");
    app.add_option("--host", host, "Server host to connect to");

    bool silent = false;
    bool spawn_server = false;
    bool launch_app = false;
    app.add_flag("--silent", silent, "Suppress startup notification");
    app.add_flag("--spawn-server", spawn_server, "Spawn a local lemond instance if none is running");
    app.add_flag("--launch-app,--open", launch_app, "Launch desktop app once server is ready");

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        return app.exit(e);
    }

    std::string clean_host;
    int clean_port = port;
    bool is_ssl = false;
    bool explicit_port = app.count("--port") > 0 || app.count("-p") > 0;
    lemon::utils::parse_target_url(host, clean_host, clean_port, is_ssl, !explicit_port);

    // Install signal handlers
    signal(SIGINT, tray_signal_handler);
    signal(SIGTERM, tray_signal_handler);
    signal(SIGPIPE, SIG_IGN);

    bool server_present = wait_for_server(clean_host, clean_port, is_ssl, 1, ServerProbeMode::allow_starting);
    bool server_initially_connected = false;

    if (!server_present && spawn_server) {
        std::string lemond_bin = find_lemond_binary(argv[0]);
        if (lemond_bin.empty()) {
            std::cerr << "Error: Could not find lemond binary." << std::endl;
            return 1;
        }

        int watchdog_pipe[2];
        if (pipe(watchdog_pipe) == -1) {
            std::cerr << "Error: Could not create watchdog pipe." << std::endl;
            return 1;
        }

        // On POSIX: pipes do not have FD_CLOEXEC set by default. Explicitly set
        // FD_CLOEXEC on the parent's write end so child processes launched by
        // the tray (e.g. desktop app or browser via xdg-open) do not inherit it
        // and keep the pipe open if the tray dies. Clear FD_CLOEXEC on the read
        // end so it survives posix_spawn in the child.
        fcntl(watchdog_pipe[1], F_SETFD, FD_CLOEXEC);
        fcntl(watchdog_pipe[0], F_SETFD, 0);

        std::string watchdog_fd_arg = "--watchdog-fd=" + std::to_string(watchdog_pipe[0]);

        std::vector<const char*> c_args;
        c_args.push_back(lemond_bin.c_str());
        c_args.push_back(watchdog_fd_arg.c_str());
        // Forward the tray's args to lemond, dropping the tray-only flags (also
        // in --flag=value form, which CLI11 accepts for booleans).
        auto is_tray_only_arg = [](const std::string& arg) {
            static const char* tray_flags[] = {"--spawn-server", "--silent", "--launch-app", "--open"};
            for (const char* flag : tray_flags) {
                std::string flag_s = flag;
                if (arg == flag_s || (arg.rfind(flag_s, 0) == 0 && arg[flag_s.size()] == '=')) {
                    return true;
                }
            }
            return false;
        };
        for (int i = 1; i < argc; ++i) {
            if (is_tray_only_arg(argv[i])) {
                continue;
            }
            c_args.push_back(argv[i]);
        }
        c_args.push_back(nullptr);

        pid_t pid;
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);

        // The child must not inherit the write end of the watchdog pipe. If it
        // does, the child's blocking read() in start_parent_watchdog() never sees
        // EOF (a live writer always exists) and an orphaned server survives the
        // tray exiting, even via SIGKILL. Closing it in the child also prevents
        // lemond from passing it down to its backend subprocesses.
        if (posix_spawn_file_actions_addclose(&actions, watchdog_pipe[1]) != 0) {
            std::cerr << "Error: Could not configure watchdog pipe." << std::endl;
            return 1;
        }

        if (posix_spawnp(&pid, lemond_bin.c_str(), &actions, nullptr, const_cast<char* const*>(c_args.data()), environ) != 0) {
            std::cerr << "Error: Could not spawn lemond." << std::endl;
            return 1;
        }
        posix_spawn_file_actions_destroy(&actions);

        close(watchdog_pipe[0]);

        std::cout << "Starting local lemond instance..." << std::endl;
        if (!wait_for_server(clean_host, clean_port, is_ssl, 15, ServerProbeMode::ready_only)) {
            std::cerr << "Error: Spawned lemond failed to start within 15 seconds." << std::endl;
            return 1;
        }
        server_initially_connected = true;
    } else if (server_present) {
        server_initially_connected = wait_for_server(clean_host, clean_port, is_ssl, 0, ServerProbeMode::ready_only);
    }

    if (!server_present && !spawn_server) {
        std::cout << "Lemonade Server is offline. Starting tray in disconnected mode..." << std::endl;
    } else if (server_present && !server_initially_connected) {
        std::cout << "Lemonade Server is starting. Starting tray in waiting mode..." << std::endl;
    }

    lemon_tray::TrayUIOptions options;
    options.port = clean_port;
    options.host = clean_host;
    options.is_ssl = is_ssl;
    options.silent = silent;
    options.launch_app = launch_app;
    options.server_initially_connected = server_initially_connected;

    lemon_tray::TrayUI tray(options);
    if (!tray.initialize()) {
        return 1;
    }

    tray.run();  // Blocks until quit

    return 0;
}

#endif
