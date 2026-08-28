#include <lemon/server.h>

#include <lemon/cli_parser.h>
#include <lemon/config_file.h>
#include <lemon/logging_config.h>
#include <lemon/system_info.h>
#include <lemon/utils/aixlog.hpp>
#include <lemon/utils/http_client.h>
#include <lemon/utils/json_utils.h>
#include <lemon/utils/path_utils.h>
#include <lemon/version.h>
#include "telemetry.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

#if defined(__GLIBC__)
#include <cstdlib>
#include <malloc.h>
#include <string_view>
#endif

#ifndef _WIN32
#include <unistd.h>
#endif

using namespace lemon;

namespace {

#if defined(__GLIBC__)
constexpr int GLIBC_MMAP_THRESHOLD_BYTES = 1024 * 1024;
constexpr std::string_view GLIBC_MMAP_THRESHOLD_TUNABLE =
    "glibc.malloc.mmap_threshold";

bool glibc_tunable_is_set(std::string_view tunables,
                          std::string_view name) noexcept {
    while (!tunables.empty()) {
        const size_t separator = tunables.find(':');
        const std::string_view entry = tunables.substr(0, separator);
        const size_t assignment = entry.find('=');
        if (assignment != std::string_view::npos &&
            entry.substr(0, assignment) == name) {
            return true;
        }
        if (separator == std::string_view::npos) {
            break;
        }
        tunables.remove_prefix(separator + 1);
    }
    return false;
}

bool glibc_mmap_threshold_is_overridden() noexcept {
    if (std::getenv("MALLOC_MMAP_THRESHOLD_") != nullptr) {
        return true;
    }
    const char* tunables = std::getenv("GLIBC_TUNABLES");
    return tunables != nullptr &&
           glibc_tunable_is_set(tunables, GLIBC_MMAP_THRESHOLD_TUNABLE);
}

bool configure_glibc_mmap_threshold() noexcept {
    if (glibc_mmap_threshold_is_overridden()) {
        return true;
    }
    // Fixing the threshold prevents glibc from retaining large temporary
    // allocations after its dynamic threshold has increased.
    return mallopt(M_MMAP_THRESHOLD, GLIBC_MMAP_THRESHOLD_BYTES) != 0;
}
#endif

} // namespace

// Global flags for signal handling
static std::atomic<bool> g_reload_requested(false);
static Server* g_server_instance = nullptr;

// Signal handler for Ctrl+C, SIGTERM, and SIGHUP
void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
#ifndef _WIN32
        const char* msg = "Shutdown signal received, exiting...\n";
        // Async-signal-safe write. The (void) cast doesn't suppress the
        // warn_unused_result attribute on glibc's write(); explicitly
        // assign-and-discard does. We genuinely don't care about partial
        // writes from inside a signal handler.
        ssize_t written = write(STDOUT_FILENO, msg, 38);
        (void)written;
#endif

        // Cancel any in-progress model download immediately. The libcurl
        // progress callback checks this flag and aborts the transfer.
        utils::g_download_cancelled.store(true);

        // Signal shutdown via the Server instance. The main loop will detect
        // this flag and call server->stop() for graceful cleanup (unloading
        // models, stopping backend child processes like llama-server).
        // This ensures child processes are properly terminated instead of
        // being orphaned when the service is stopped via systemd.
        if (g_server_instance) {
            g_server_instance->set_shutdown_requested(true);
        }

        // Return normally instead of calling _exit(). The main loop will
        // detect the flag, call stop() to clean up child processes, and
        // then exit. This prevents orphaned backend processes.
        return;
#ifdef SIGHUP
    } else if (signal == SIGHUP) {
        // Set the reload flag; a background thread will call invalidate_recipes().
        // Calling mutex-based code directly from a signal handler is not async-signal-safe.
        g_reload_requested = true;
#endif
    }
}

int main(int argc, char** argv) {
#if defined(__GLIBC__)
    // mallopt() is process-wide and MT-Unsafe. Apply it before initializing
    // telemetry or constructing Server, both of which start worker threads.
    if (!configure_glibc_mmap_threshold()) {
        std::cerr << "Warning: failed to set glibc M_MMAP_THRESHOLD; "
                  << "memory from large requests may not be released promptly"
                  << std::endl;
    }
#endif

    telemetry::initialize();

    try {
        CLIParser parser;
        parser.parse(argc, argv);

        if (!parser.should_continue()) {
            return parser.get_exit_code();
        }

        auto cli_config = parser.get_config();

        // Initialize logging early with INFO so config loading messages are captured
        {
            auto early_filter = AixLog::Filter(AixLog::Severity::info);
            auto early_sink = std::make_shared<AixLog::SinkCout>(early_filter, RuntimeConfig::LOG_FORMAT);
            AixLog::Log::init({early_sink});
        }

        utils::set_cache_dir(cli_config.cache_dir);
        utils::set_config_dir(cli_config.config_dir);
        utils::migrate_legacy_paths(cli_config.cache_dir, cli_config.config_dir);
        json config_json = ConfigFile::load(cli_config.cache_dir,
                                            cli_config.config_dir);

        auto config = std::make_shared<RuntimeConfig>(config_json);
        RuntimeConfig::set_global(config.get());

        if (cli_config.port != -1) {
            config->set_port_override(cli_config.port);
        }
        if (!cli_config.host.empty()) {
            config->set_host_override(cli_config.host);
        }
        if (cli_config.broadcast.has_value()) {
            config->set_broadcast_override(cli_config.broadcast);
        }
        if (!cli_config.log_file.empty()) {
            config->set_log_file_override(cli_config.log_file);
        }
        if (cli_config.log_max_file_size_mb != -1) {
            config->set_log_max_file_size_mb_override(cli_config.log_max_file_size_mb);
        }
        if (cli_config.log_max_files != -1) {
            config->set_log_max_files_override(cli_config.log_max_files);
        }

        // Initialize logging with configured level and rotation limits
        LogRotationConfig rot_cfg;
        rot_cfg.file_mode = config->log_file();
        rot_cfg.max_file_size_mb = static_cast<size_t>(config->log_max_file_size_mb());
        rot_cfg.max_files = static_cast<size_t>(config->log_max_files());
        configure_application_logging(config->log_level(), LoggingMode::direct_server, rot_cfg);

        utils::set_models_dir(config->models_dir());

        LOG(INFO) << "Starting Lemonade Server..." << std::endl;
        LOG(INFO) << "  Version: " << LEMON_VERSION_STRING << std::endl;
        LOG(INFO) << "  Cache dir: " << cli_config.cache_dir << std::endl;
        LOG(INFO) << "  Config dir: " << cli_config.config_dir << std::endl;
        LOG(INFO) << "  Port: " << config->port() << std::endl;
        LOG(INFO) << "  Host: " << config->host() << std::endl;
        LOG(INFO) << "  Log level: " << config->log_level() << std::endl;
        LOG(INFO) << "  Log file mode: " << config->log_file() << std::endl;
        LOG(INFO) << "  Log max file size: " << config->log_max_file_size_mb() << " MB" << std::endl;
        LOG(INFO) << "  Log max files: " << config->log_max_files() << std::endl;
        if (!config->extra_models_dir().empty()) {
            LOG(INFO) << "  Extra models dir: " << config->extra_models_dir() << std::endl;
        }
        if (config->telemetry_enabled()) {
            std::string endpoint = config->telemetry_otlp_endpoint();
            std::vector<std::string> semantics = config->telemetry_otlp_semantics();
            std::string semantics_str = "";
            for (size_t i = 0; i < semantics.size(); ++i) {
                if (i > 0) semantics_str += ", ";
                semantics_str += semantics[i];
            }
            if (endpoint.empty()) {
                LOG(INFO) << "  Telemetry: enabled (no endpoint configured, semantics: [" << semantics_str << "])" << std::endl;
            } else {
                LOG(INFO) << "  Telemetry: enabled (" << config->telemetry_otlp_protocol()
                          << " -> " << endpoint << ", semantics: [" << semantics_str << "])" << std::endl;
            }
        } else {
            LOG(INFO) << "  Telemetry: disabled" << std::endl;
        }

        Server server(config, cli_config.cache_dir, cli_config.config_dir);

        g_server_instance = &server;
        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);
#ifdef SIGHUP
        std::signal(SIGHUP, signal_handler);

        // Background thread: watches g_reload_requested and calls invalidate_recipes().
        // Mutex-based code (like invalidate_recipes) must not be called directly from
        // a signal handler, so we use this thread to do the actual work safely.
        std::thread([]() {
            while (!g_server_instance || !g_server_instance->should_shutdown()) {
                if (g_reload_requested.exchange(false)) {
                    LOG(INFO) << "SIGHUP received - rescanning hardware and recipes..." << std::endl;
                    SystemInfoCache::invalidate_recipes();
                    LOG(INFO) << "Hardware rescan complete" << std::endl;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        }).detach();
#endif

        server.run();
        g_server_instance = nullptr;

        // Startup aborted (e.g. port already in use): exit non-zero now and skip
        // destructors, whose teardown logging would bury the error message.
        if (server.startup_failed()) {
            std::cout.flush();
            std::cerr.flush();
            std::_Exit(1);
        }

        return 0;

    } catch (const std::exception& e) {
        LOG(ERROR) << "Error: " << e.what() << std::endl;
        return 1;
    }
}
