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
#include <lemon/cli_parser.h>
#include <lemon/server.h>
#include <lemon/single_instance.h>
#include <lemon/utils/aixlog.hpp>
#include <lemon/version.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <CLI/CLI.hpp>
#include <httplib.h>

#ifdef _WIN32
// Windows embeds the server
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

static bool wait_for_server(const std::string& host, int port, int timeout_seconds) {
    std::string connect_host = (host.empty() || host == "0.0.0.0" || host == "localhost")
        ? "127.0.0.1" : host;

    for (int i = 0; i < timeout_seconds * 2; ++i) {
        try {
            httplib::Client cli(connect_host, port);
            cli.set_connection_timeout(1);
            cli.set_read_timeout(5);
            // Use /api/v1/health instead of /live — /live responds before the model
            // cache is built, which causes 500s on /models if clients connect too early.
            auto res = cli.Get("/api/v1/health");
            if (res && res->status == 200) {
                return true;
            }
        } catch (...) {}
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
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
    auto config = parser.get_config();

    // Initialize logging to file (SUBSYSTEM:WINDOWS has no console).
    // The server's /api/v1/logs/stream endpoint tails this same file.
    {
        char temp_path[MAX_PATH];
        GetTempPathA(MAX_PATH, temp_path);
        std::string log_file = std::string(temp_path) + "lemonade-server.log";
        auto file_sink = std::make_shared<AixLog::SinkFile>(
            AixLog::Filter(AixLog::to_severity(config.log_level)),
            log_file,
            lemon::RuntimeConfig::LOG_FORMAT);
        AixLog::Log::init({file_sink});
    }

    // Initialize Winsock (required by httplib)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    // Start server on background thread (capture config by value — thread outlives the stack frame)
    std::thread server_thread([config]() {
        try {
            lemon::Server server(config.port, config.host, config.log_level,
                                config.recipe_options, config.max_loaded_models,
                                config.extra_models_dir, config.no_broadcast,
                                config.global_timeout);
            server.run();
        } catch (const std::exception& e) {
            MessageBoxA(NULL, e.what(), "Lemonade Server Error", MB_OK | MB_ICONERROR);
        }
    });
    server_thread.detach();

    // Wait for server to be ready
    if (!wait_for_server(config.host, config.port, 15)) {
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
        lemon_tray::TrayUI tray(config.port, config.host, silent);
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
        std::string connect_host = (config.host.empty() || config.host == "0.0.0.0" || config.host == "localhost")
            ? "127.0.0.1" : config.host;
        httplib::Client cli(connect_host, config.port);
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

// Signal handler writes to self-pipe for clean shutdown
static void tray_signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        char c = (char)sig;
        ssize_t written = write(lemon_tray::TrayUI::signal_pipe_[1], &c, 1);
        (void)written;
    }
}

int main(int argc, char* argv[]) {
    // Single instance check
    if (lemon::SingleInstance::IsAnotherInstanceRunning("Tray")) {
        std::cerr << "lemonade-tray is already running." << std::endl;
        return 0;
    }

    // Parse args
    CLI::App app{"Lemonade Tray - system tray interface for Lemonade Server"};
    int port = 8000;
    std::string host = "localhost";

    app.add_option("--port,-p", port, "Server port to connect to");
    app.add_option("--host", host, "Server host to connect to");

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        return app.exit(e);
    }

    // Install signal handlers
    signal(SIGINT, tray_signal_handler);
    signal(SIGTERM, tray_signal_handler);

    // Wait for router to be reachable (retry with backoff up to 30s)
    std::cout << "Connecting to lemond at " << host << ":" << port << "..." << std::endl;
    if (!wait_for_server(host, port, 30)) {
        std::cerr << "Error: Could not connect to lemond at " << host << ":" << port << std::endl;
        std::cerr << "Make sure lemond is running." << std::endl;
        return 1;
    }

    std::cout << "Connected to lemond v" << LEMON_VERSION_STRING << std::endl;

    // Create and run tray UI
    lemon_tray::TrayUI tray(port, host);
    if (!tray.initialize()) {
        return 1;
    }

    tray.run();  // Blocks until quit

    // On macOS/Linux, just exit — the router keeps running
    return 0;
}

#endif
