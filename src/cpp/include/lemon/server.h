#pragma once

// Define thread pool count BEFORE including httplib.h. This is only the
// fallback for httplib's default-constructed servers; the listeners are sized
// at runtime from the host CPU count in setup_http_servers().
#ifndef CPPHTTPLIB_THREAD_POOL_COUNT
#define CPPHTTPLIB_THREAD_POOL_COUNT 64
#endif

#include <string>
#include <thread>
#include <memory>
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <vector>
#include <httplib.h>
#include "runtime_config.h"
#include "router.h"
#include "routing_policy.h"
#include "model_manager.h"
#include "backend_manager.h"
#include "cloud_provider_registry.h"
#include "upgradable_http_server.h"
#include "websocket_server.h"
#include "lemon/utils/network_beacon.h"
#include "lemon/system_metrics_platform.h"

namespace lemon {

// Forward declaration
class SystemMetricsPlatform;

struct RouterDispatchResult {
    std::string requested_model;
    std::string selected_model;
    Decision decision;
};

class Server {
public:
    Server(std::shared_ptr<RuntimeConfig> config, const std::string& cache_dir);

    ~Server();

    // Start the server
    void run();

    // Stop the server
    void stop();

    // Check if shutdown has been requested (for use by the main loop)
    bool should_shutdown() const;

    // Signal that shutdown has been requested (called by signal handler)
    void set_shutdown_requested(bool requested);

    // Get server status
    bool is_running() const;

    // True if run() aborted startup (e.g. the port was already in use), so
    // main() can report failure and exit non-zero.
    bool startup_failed() const;

private:
    std::string resolve_host_to_ip(int ai_family, const std::string& host);
    void setup_routes(httplib::Server &web_server);
    void setup_static_files(httplib::Server &web_server);
    void setup_cors(httplib::Server &web_server);
    void setup_http_logger(httplib::Server &web_server);
    void log_request(const httplib::Request& req);
    httplib::Server::HandlerResponse authenticate_request(const httplib::Request& req, httplib::Response& res);

    // Setup HTTP servers (create httplib::Server instances, routes, CORS, thread pool)
    void setup_http_servers();

    // Stop the main-port listeners (fronts) and detach the routed servers
    void stop_http_listeners();

    // Unified config endpoints
    void handle_config_set(const httplib::Request& req, httplib::Response& res);
    void handle_config_get(const httplib::Request& req, httplib::Response& res);
    void handle_config_defaults_get(const httplib::Request& req, httplib::Response& res);

    // Side-effect callback for RuntimeConfig::set(). Receives a nested JSON
    // mirroring the input shape, containing only entries that actually changed.
    void apply_config_side_effects(const json& applied_changes);

    // Hot-swap a backend binary when its *_bin config value changes. Unloads
    // affected loaded models, runs install_backend (which downloads/replaces
    // when version.txt mismatches), then best-effort reloads them. Errors are
    // logged and never propagated — the config change has already been applied.
    void handle_bin_change(const std::string& section,
                           const std::string& bin_key,
                           const std::string& new_value);

    // Endpoint handlers
    void handle_health(const httplib::Request& req, httplib::Response& res);
    void handle_live(const httplib::Request& req, httplib::Response& res);
    void handle_models(const httplib::Request& req, httplib::Response& res);
    void handle_model_by_id(const httplib::Request& req, httplib::Response& res);
    void handle_model_update_check(const httplib::Request& req, httplib::Response& res);
    void handle_model_files(const httplib::Request& req, httplib::Response& res);
    void handle_chat_completions(const httplib::Request& req, httplib::Response& res);
    // Server-side tool-calling orchestration for Omni "collection" models.
    void handle_collection_chat_completions(const nlohmann::json& request_json,
                                            const ModelInfo& collection_info,
                                            httplib::Response& res);
    // Run a collection.router model's routing engine and return the selected
    // candidate plus the full Decision. Returns std::nullopt when routing did
    // not engage (no parsed policy), so callers leave the request untouched.
    std::optional<RouterDispatchResult> route_collection_request(
        const nlohmann::json& request_json,
        const ModelInfo& collection_info);
    // If request_json addresses a collection.router model, rewrite its "model"
    // field in place to the engine-selected candidate and return the Decision.
    // No-op otherwise.
    std::optional<RouterDispatchResult> apply_router_collection_dispatch(
        nlohmann::json& request_json);
    void handle_completions(const httplib::Request& req, httplib::Response& res);
    void handle_embeddings(const httplib::Request& req, httplib::Response& res);
    void handle_reranking(const httplib::Request& req, httplib::Response& res);
    void handle_slots(const httplib::Request& req, httplib::Response& res);
    void handle_slots_by_id(const httplib::Request& req, httplib::Response& res);
    void handle_tokenize(const httplib::Request& req, httplib::Response& res);
    void handle_responses(const httplib::Request& req, httplib::Response& res);
    void handle_pull(const httplib::Request& req, httplib::Response& res);
    void handle_pull_variants(const httplib::Request& req, httplib::Response& res);
    void handle_load(const httplib::Request& req, httplib::Response& res);
    void handle_unload(const httplib::Request& req, httplib::Response& res);
    void handle_pin(const httplib::Request& req, httplib::Response& res);
    void handle_delete(const httplib::Request& req, httplib::Response& res);
    void handle_cleanup_cache(const httplib::Request& req, httplib::Response& res);

    // Cloud auth (public, all four prefixes).
    //   POST /v1/cloud/auth   body: {provider, api_key}
    //     -> store key in process memory for that provider, refresh the
    //        provider's discovered model list. Returns 409 if the
    //        provider's env var is set (env wins).
    //   DELETE /v1/cloud/auth/{provider}
    //     -> clear the in-memory runtime key (env var unaffected).
    // Admin-gated only when LEMONADE_ADMIN_API_KEY is explicitly set, same
    // gate as /internal/shutdown — matches the existing pattern so dev
    // loops without any keys still work.
    void handle_cloud_auth_set(const httplib::Request& req, httplib::Response& res);
    void handle_cloud_auth_clear(const httplib::Request& req, httplib::Response& res);
    void handle_params(const httplib::Request& req, httplib::Response& res);
    void handle_metrics(const httplib::Request& req, httplib::Response& res);
    void handle_stats(const httplib::Request& req, httplib::Response& res);
    void handle_system_info(const httplib::Request& req, httplib::Response& res);
    void handle_system_stats(const httplib::Request& req, httplib::Response& res);
    void handle_log_level(const httplib::Request& req, httplib::Response& res);
    void handle_shutdown(const httplib::Request& req, httplib::Response& res);
    void handle_simulate_vram_pressure(const httplib::Request& req, httplib::Response& res);

    // Backend management endpoint handlers
    void handle_install(const httplib::Request& req, httplib::Response& res);
    void handle_install_dry_run(const httplib::Request& req, httplib::Response& res);
    void handle_uninstall(const httplib::Request& req, httplib::Response& res);

    // Enrich recipes JSON with release_url, download_filename, version from BackendManager
    void enrich_recipes(json& recipes);

    // Download manager endpoints and server-owned download jobs
    void handle_downloads(const httplib::Request& req, httplib::Response& res);
    void handle_download_control(const httplib::Request& req, httplib::Response& res);

    // Shared SSE streaming helper for legacy download operations. The operation
    // remains tied to this response for backwards compatibility.
    void stream_download_operation(
        httplib::Response& res,
        std::function<void(DownloadProgressCallback)> operation);

    struct DownloadJob {
        std::string id;
        std::string type;
        std::string display_name;
        std::string status;
        std::string cancel_action;
        std::string error;
        nlohmann::json progress;
        uint64_t completed_files_bytes = 0;
        uint64_t current_file_bytes_total = 0;
        int current_file_index = -1;
        bool cancel_requested = false;
        // Set by the worker's progress callback once the downloader has actually
        // observed a pause/cancel request and stopped before completion. This
        // prevents a late UI request from overriding a successful operation that
        // already returned normally.
        bool stop_acknowledged = false;
        bool running = false;
        std::chrono::steady_clock::time_point terminal_since;
        // Protects worker publication/join. A job can be visible in the registry
        // while start_download_job is still joining the previous worker; removals
        // and shutdown must wait until the new worker thread is either assigned
        // or known to be absent before deciding whether to join.
        mutable std::mutex worker_mutex;
        std::thread worker;
    };

    nlohmann::json download_progress_to_json(const DownloadProgress& progress);
    nlohmann::json download_job_to_json(const std::shared_ptr<DownloadJob>& job);
    bool is_download_job_visible(const std::shared_ptr<DownloadJob>& job) const;
    std::shared_ptr<DownloadJob> start_download_job(
        const std::string& download_id,
        const std::string& download_type,
        const std::string& display_name,
        std::function<void(DownloadProgressCallback)> operation);
    void join_download_job(const std::shared_ptr<DownloadJob>& job);
    void cancel_download_jobs();

    // Helper function for local model resolution and registration
    void resolve_and_register_local_model(
        const std::string& dest_path,
        const std::string& model_name,
        const json& model_data,
        const std::string& hf_cache);

    // Audio endpoint handlers (OpenAI /v1/audio/* compatible)
    void handle_audio_transcriptions(const httplib::Request& req, httplib::Response& res);
    void handle_audio_speech(const httplib::Request& req, httplib::Response& res);

    // Image endpoint handlers (OpenAI /v1/images/* compatible)
    void handle_image_generations(const httplib::Request& req, httplib::Response& res);
    void handle_image_edits(const httplib::Request& req, httplib::Response& res);
    void handle_image_variations(const httplib::Request& req, httplib::Response& res);
    void handle_image_upscale(const httplib::Request& req, httplib::Response& res);

    // Generative-audio endpoint handler (text -> audio clip: music, SFX)
    void handle_audio_generations(const httplib::Request& req, httplib::Response& res);
    void handle_3d_generations(const httplib::Request& req, httplib::Response& res);

    // Run a media generation into a buffer and respond: the bytes on success, or an
    // HTTP error if the backend produced nothing (it crashed / OOM'd / failed). This
    // avoids returning a 200 with an empty body that looks like a successful empty file.
    void serve_media_or_error(httplib::Response& res, const std::string& mime_type,
                              const std::function<void(httplib::DataSink&)>& generate);

    // Shared helpers for image multipart handlers
    // Return true on success; on failure set res status/body and return false.
    bool parse_n_from_form(const httplib::Request& req, httplib::Response& res, nlohmann::json& out);
    bool extract_image_from_form(const httplib::Request& req, httplib::Response& res, nlohmann::json& out);
    bool load_image_model(const nlohmann::json& request_json, httplib::Response& res);

    bool parse_required_json_body(const httplib::Request& req,
                                  httplib::Response& res,
                                  nlohmann::json& out);

    // Auto-load a model on first use. request_options are applied only on the first load;
    // if the model is already loaded they are ignored so explicit /v1/load settings win.
    // Callers must pass only load-level options from extract_auto_load_options() — never
    // the raw request body — to keep request-scoped fields out of persistent recipe options.
    void auto_load_model_if_needed(const std::string& model_name,
                                   const json& request_options = json::object());

    // Helper: persist the registry's installed-providers list into config.json
    // by overlaying onto the current runtime-config snapshot. Called after
    // install/uninstall. Errors are logged and swallowed — a failure to
    // persist must not prevent the in-memory state change that already
    // happened.
    void persist_cloud_providers();

    // Load every component of a collection (Omni) model, downloading any that are
    // missing. Shared by handle_load and auto_load_model_if_needed.
    void ensure_collection_loaded(const ModelInfo& info);

    // Helper function to convert ModelInfo to JSON (used by models endpoints).
    // `depth` tracks collection-component nesting; embedding stops past
    // kMaxCollectionEmbedDepth so a cyclic collection registration cannot
    // recurse unboundedly.
    nlohmann::json model_info_to_json(const std::string& model_id, const ModelInfo& info,
                                      int depth = 0);

    // Warm model list cache in the background after startup dependencies are initialized
    void start_model_cache_warmup();

    // Helper function to generate detailed model error responses (not found, not supported, load failure)
    nlohmann::json create_model_error(const std::string& requested_model, const std::string& exception_msg);
    // System stats helper methods
    double get_cpu_usage();
    double get_gpu_usage();
    double get_vram_usage();
    double get_npu_utilization();

    std::shared_ptr<RuntimeConfig> config_;
    std::string cache_dir_;  // Lemonade cache dir for config.json persistence
    std::atomic<int> port_;  // Atomic cache for lock-free reads from listener threads

    std::thread http_v4_thread_;
    std::thread http_v6_thread_;
    std::thread model_cache_warmup_thread_;


    // Routed servers (all routes/handlers; never listen) and the main-port
    // front listeners that feed them — see upgradable_http_server.h
    std::unique_ptr<RoutedHttpServer> http_server_;
    std::unique_ptr<RoutedHttpServer> http_server_v6_;
    std::unique_ptr<UpgradableFrontServer> http_front_;
    std::unique_ptr<UpgradableFrontServer> http_front_v6_;

    std::unique_ptr<Router> router_;
    std::unique_ptr<ModelManager> model_manager_;
    std::unique_ptr<BackendManager> backend_manager_;
    std::unique_ptr<CloudProviderRegistry> cloud_registry_;
    std::unique_ptr<WebSocketServer> websocket_server_;

    std::mutex downloads_mutex_;
    std::map<std::string, std::shared_ptr<DownloadJob>> download_jobs_;

    bool running_;
    bool startup_failed_ = false;
    std::atomic<bool> shutdown_requested_{false};
    std::atomic<bool> rebind_requested_{false};
    std::atomic<bool> metrics_access_logged_{false};

    std::string api_key_;
    std::string admin_api_key_;
    NetworkBeacon udp_beacon_;

    // CPU usage tracking
#if defined(__linux__) || defined(_WIN32)
    struct CpuStats {
        uint64_t total_idle = 0;
        uint64_t total = 0;
    };
    CpuStats last_cpu_stats_;
    std::mutex cpu_stats_mutex_;
#endif

    // Platform-specific system metrics
    std::unique_ptr<SystemMetricsPlatform> metrics_platform_;

    // Set to true after check_for_model_updates() completes at startup.
    std::atomic<bool> update_check_done_{false};

    // Extract load-level options from an inference request body. Currently only ctx_size
    // is forwarded; request-scoped fields are excluded so they cannot leak into recipe options.
    static json extract_auto_load_options(const json& request);
};

} // namespace lemon
