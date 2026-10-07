#pragma once

// Define thread pool count BEFORE including httplib.h. This is only the
// fallback for httplib's default-constructed servers; the listeners are sized
// at runtime from the host CPU count in HttpListener.
#ifndef CPPHTTPLIB_THREAD_POOL_COUNT
#define CPPHTTPLIB_THREAD_POOL_COUNT 64
#endif

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <httplib.h>

#include "lemon/runtime_config.h"
#include "lemon/server/server_context.h"

namespace lemon {

class RequestMiddleware;
class WebUi;

// lemond's HTTP server. It builds the subsystems and the ServerContext routes use,
// registers every route folder, and starts and stops the core components in order.
class Server {
public:
    Server(std::shared_ptr<RuntimeConfig> config,
           const std::string& cache_dir,
           const std::string& config_dir);

    ~Server();

    // Serves until stop() or a shutdown request.
    void run();

    // Stops the listeners, unloads every model and flushes telemetry. Safe to call from
    // any thread, more than once.
    void stop();

    // Main's loop and signal handler use these to request and observe a shutdown.
    bool should_shutdown() const;
    void set_shutdown_requested(bool requested);

    bool is_running() const;

    // True if run() aborted startup (e.g. the port was already in use), so
    // main() can report failure and exit non-zero.
    bool startup_failed() const;

    // The directory lemond was started with. Tests start lemond in a temporary one, so
    // routes persist config.json here rather than in utils::get_config_dir().
    const std::string& config_dir() const { return config_dir_; }

    // Set once the startup model update check completes; /health reports it.
    bool update_check_done() const { return update_check_done_.load(); }

    // Serves one request in-process, as the route that would match it on the socket.
    // Answers 404 when no route matches.
    void handle_request(const httplib::Request& req, httplib::Response& res);

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
    std::atomic<bool> update_check_done_{false};

    std::mutex stop_mutex_;
};

} // namespace lemon
