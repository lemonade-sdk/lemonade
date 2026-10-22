#pragma once

// Define thread pool count BEFORE including httplib.h. This is only the
// fallback for httplib's default-constructed servers; the listeners are sized
// at runtime from the host CPU count in HttpListener.
#ifndef CPPHTTPLIB_THREAD_POOL_COUNT
#define CPPHTTPLIB_THREAD_POOL_COUNT 64
#endif

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <httplib.h>

#include "lemon/runtime_config.h"
#include "lemon/server/server_context.h"

namespace lemon {

class RequestMiddleware;
class WebUi;

class Server {
public:
    Server(std::shared_ptr<RuntimeConfig> config,
           const std::string& cache_dir,
           const std::string& config_dir);

    ~Server();

    // Serves until stop() or a shutdown request.
    void run();

    // Safe to call from any thread, more than once.
    void stop();

    bool should_shutdown() const;
    void set_shutdown_requested(bool requested);

    bool is_running() const;

    // True if run() aborted startup (e.g. the port was already in use), so
    // main() can report failure and exit non-zero.
    bool startup_failed() const;

    // Tests start lemond in a temporary config dir, so routes persist config.json here
    // rather than in utils::get_config_dir().
    const std::string& config_dir() const { return config_dir_; }

    // Set once the startup model update check completes.
    bool update_check_done() const { return update_check_done_.load(); }

    // Serves one request in-process, as the route that would match it on the socket.
    // Answers 404 when no route matches.
    void handle_request(const httplib::Request& req, httplib::Response& res);

    // Runs the queued model sync on a thread that stop() joins.
    void start_background_sync();

    // CPU usage percentage (0-100) since the previous call, -1 if not available.
    double get_cpu_usage();

private:
    void start_model_cache_warmup();

    // Declared so that destruction runs the listeners first and the subsystems last.
    std::shared_ptr<RuntimeConfig> config_;
    std::string config_dir_;
    std::unique_ptr<Router> router_;
    std::unique_ptr<AliasManager> alias_manager_;
    std::unique_ptr<ModelManager> model_manager_;
    std::unique_ptr<BackendManager> backend_manager_;
    std::unique_ptr<CloudProviderRegistry> cloud_registry_;
    std::unique_ptr<SystemMetricsPlatform> metrics_platform_;
    std::unique_ptr<ModelJson> model_json_;
    std::unique_ptr<ModelLoader> model_loader_;
    std::unique_ptr<DownloadManager> downloads_;
    std::unique_ptr<jobs::JobManager> job_manager_;
    ServerContext ctx_;
    std::unique_ptr<ConfigEffects> config_effects_;
    std::unique_ptr<RouteRegistry> registry_;
    std::unique_ptr<RequestMiddleware> middleware_;
    std::unique_ptr<WebUi> web_ui_;
    std::unique_ptr<HttpListener> listener_;

    std::thread model_cache_warmup_thread_;
    struct SyncTaskThread {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> finished;
    };
    std::vector<SyncTaskThread> background_sync_threads_;
    std::mutex background_sync_mutex_;

#if defined(__linux__) || defined(_WIN32)
    struct CpuStats {
        uint64_t total_idle = 0;
        uint64_t total = 0;
    };
    CpuStats last_cpu_stats_;
    std::mutex cpu_stats_mutex_;
#endif
    std::atomic<bool> update_check_done_{false};

    std::mutex stop_mutex_;
};

} // namespace lemon
