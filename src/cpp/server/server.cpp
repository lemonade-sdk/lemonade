#include "lemon/server.h"

#include <lemon/utils/aixlog.hpp>

#include "lemon/backend_manager.h"
#include "lemon/cloud_provider_registry.h"
#include "lemon/jobs/job_manager.h"
#include "lemon/model_manager.h"
#include "lemon/router.h"
#include "lemon/runtime_config.h"
#include "lemon/server/config_effects.h"
#include "lemon/server/download_manager.h"
#include "lemon/server/http_listener.h"
#include "lemon/server/job_ops.h"
#include "lemon/server/model_json.h"
#include "lemon/server/model_loader.h"
#include "lemon/server/request_middleware.h"
#include "lemon/server/route_registry.h"
#include "lemon/server/router_dispatch.h"
#include "lemon/server/web_ui.h"
#include "lemon/system_metrics_platform.h"
#include "lemon/utils/http_client.h"
#include "telemetry.h"

namespace lemon {

// Each route folder's routes.cpp adds its routes in the order httplib should match them.
void register_openai_text_routes(RouteRegistry& registry, ServerContext& ctx);
void register_openai_audio_routes(RouteRegistry& registry, ServerContext& ctx);
void register_openai_images_routes(RouteRegistry& registry, ServerContext& ctx);
void register_lemonade_models_routes(RouteRegistry& registry, ServerContext& ctx);
void register_openai_models_routes(RouteRegistry& registry, ServerContext& ctx);
void register_lemonade_load_routes(RouteRegistry& registry, ServerContext& ctx);
void register_lemonade_downloads_routes(RouteRegistry& registry, ServerContext& ctx);
void register_lemonade_inference_routes(RouteRegistry& registry, ServerContext& ctx);
void register_lemonade_backends_routes(RouteRegistry& registry, ServerContext& ctx);
void register_lemonade_system_routes(RouteRegistry& registry, ServerContext& ctx);
void register_lemonade_jobs_routes(RouteRegistry& registry, ServerContext& ctx);
void register_llamacpp_routes(RouteRegistry& registry, ServerContext& ctx);
void register_router_routes(RouteRegistry& registry, ServerContext& ctx);
void register_internal_routes(RouteRegistry& registry, ServerContext& ctx);
void register_ollama_routes(RouteRegistry& registry, ServerContext& ctx);
void register_anthropic_routes(RouteRegistry& registry, ServerContext& ctx);
void register_mcp_routes(RouteRegistry& registry, ServerContext& ctx);

Server::Server(std::shared_ptr<RuntimeConfig> config,
               const std::string& cache_dir,
               const std::string& config_dir)
    : config_(config), config_dir_(config_dir) {
    ctx_.server = this;
    ctx_.config = config_.get();

    config_effects_ = std::make_unique<ConfigEffects>(ctx_);
    ctx_.config_effects = config_effects_.get();
    utils::HttpClient::set_default_timeout(config_->global_timeout());
    utils::HttpClient::set_download_rate_limit(config_->download_rate_limit_bytes_per_second());

    cloud_registry_ = std::make_unique<CloudProviderRegistry>();
    // Seed installed providers from config.json. Runtime keys stay empty
    // until either an env var resolves them per-request or a client POSTs
    // /v1/cloud/auth — by design we never persist secrets to disk.
    {
        nlohmann::json snap = config_->snapshot();
        if (snap.contains("cloud_providers")) {
            cloud_registry_->load_from_config(snap["cloud_providers"]);
        }
    }

    ModelManagerConfig manager_config;
    manager_config.cache_dir = cache_dir;
    manager_config.extra_models_dir = config_->extra_models_dir();
    manager_config.cloud_providers = cloud_registry_.get();
    manager_config.default_model_source = [this]() { return config_->default_model_source(); };
    manager_config.model_catalogs = [this]() { return config_->model_catalogs(); };
    model_manager_ = std::make_unique<ModelManager>(std::move(manager_config));

    backend_manager_ = std::make_unique<BackendManager>();
    BackendManager::set_global(backend_manager_.get());

    router_ = std::make_unique<Router>(config_.get(),
                                       model_manager_.get(),
                                       backend_manager_.get());
    router_->set_cloud_registry(cloud_registry_.get());

    // When a router collection is added, edited, or removed (via the API or an
    // on-disk edit), reclaim any routing helper no remaining policy references.
    model_manager_->on_models_changed([this](uint64_t generation) {
        router_->reconcile_routing_helpers(model_manager_->active_helper_models(), generation);
    });

    // Reconcile once at startup so a helper loaded before the first policy
    // change still validates against an authoritative set (see
    // Router::load_model). The helper set and its generation come from one
    // published snapshot, so they cannot disagree the way two separate reads
    // could.
    model_manager_->notify_models_changed();

    model_manager_->on_model_updated([this](const std::string& model_name) {
        if (router_ && router_->is_model_loaded(model_name)) {
            LOG(INFO, "Server") << "Evicting updated model from memory: " << model_name << std::endl;
            router_->unload_model(model_name);
        }
    });

    metrics_platform_ = create_metrics_platform();
    model_json_ = std::make_unique<ModelJson>(model_manager_.get(), router_.get());
    model_loader_ = std::make_unique<ModelLoader>(router_.get(), model_manager_.get());
    downloads_ = std::make_unique<DownloadManager>();
    backend_manager_->set_download_manager(downloads_.get());

    ctx_.router = router_.get();
    ctx_.model_manager = model_manager_.get();
    ctx_.backend_manager = backend_manager_.get();
    ctx_.cloud_registry = cloud_registry_.get();
    ctx_.metrics = metrics_platform_.get();
    ctx_.model_json = model_json_.get();
    ctx_.model_loader = model_loader_.get();
    ctx_.downloads = downloads_.get();

    job_manager_ = std::make_unique<lemon::jobs::JobManager>(
        cache_dir, config_dir, lemon::jobs::build_op_registry(JobOps::build(ctx_)));
    ctx_.job_manager = job_manager_.get();

    LOG(DEBUG, "Server") << "Debug logging enabled - subprocess output will be visible" << std::endl;

    registry_ = std::make_unique<RouteRegistry>();
    ctx_.registry = registry_.get();
    // Pages list routes in registration order. lemonade/models adds the 405 answers for
    // GET models/register and models/check-updates, which must precede openai/models'
    // models/{id} pattern.
    register_openai_text_routes(*registry_, ctx_);
    register_openai_audio_routes(*registry_, ctx_);
    register_openai_images_routes(*registry_, ctx_);
    register_lemonade_models_routes(*registry_, ctx_);
    register_openai_models_routes(*registry_, ctx_);
    register_lemonade_load_routes(*registry_, ctx_);
    register_lemonade_downloads_routes(*registry_, ctx_);
    register_lemonade_inference_routes(*registry_, ctx_);
    register_lemonade_backends_routes(*registry_, ctx_);
    register_lemonade_system_routes(*registry_, ctx_);
    register_lemonade_jobs_routes(*registry_, ctx_);
    register_llamacpp_routes(*registry_, ctx_);
    register_router_routes(*registry_, ctx_);
    register_internal_routes(*registry_, ctx_);
    register_ollama_routes(*registry_, ctx_);
    register_anthropic_routes(*registry_, ctx_);
    register_mcp_routes(*registry_, ctx_);

    middleware_ = std::make_unique<RequestMiddleware>(config_.get(), registry_.get());
    web_ui_ = std::make_unique<WebUi>(ctx_);
    listener_ = std::make_unique<HttpListener>(
        config_.get(), router_.get(),
        [this](httplib::Server& server) {
            middleware_->install(server);
            registry_->apply(server);
            web_ui_->register_routes(server);
        },
        [this]() { stop(); }, middleware_->api_key_set(), middleware_->admin_api_key_set());
    ctx_.listener = listener_.get();

    start_model_cache_warmup();
}

Server::~Server() {
    downloads_->cancel_all();
    model_manager_->cancel_sync();
    stop();
}

void Server::start_model_cache_warmup() {
    if (model_cache_warmup_thread_.joinable()) {
        return;
    }

    model_cache_warmup_thread_ = std::thread([this]() {
        try {
            LOG(DEBUG, "Server") << "Warming model list cache..." << std::endl;
            model_manager_->snapshot();
            LOG(DEBUG, "Server") << "Model list cache warmup complete" << std::endl;
        } catch (const std::exception& e) {
            LOG(WARNING, "Server") << "Model list cache warmup failed: " << e.what() << std::endl;
        } catch (...) {
            LOG(WARNING, "Server") << "Model list cache warmup failed with unknown error" << std::endl;
        }

        if (config_->auto_check_model_updates()) {
            try {
                LOG(DEBUG, "Server") << "Checking downloaded models for updates..." << std::endl;
                auto check_res = model_manager_->check_updates();
                LOG(DEBUG, "Server") << "Model update check complete" << std::endl;

                for (const auto& [model_name, err] : check_res.failed_models) {
                    LOG(WARNING, "Server") << "Model update check failed for " << model_name << ": " << err << std::endl;
                }

                std::vector<std::string> auto_update_targets;
                for (const auto& public_name : check_res.updated_models) {
                    try {
                        std::string canonical_name = model_manager_->resolve_model_name(public_name);
                        ModelInfo info = model_manager_->get_model_info(canonical_name);
                        if (model_manager_->should_auto_update(info)) {
                            auto_update_targets.push_back(public_name);
                        }
                    } catch (...) {}
                }

                if (!auto_update_targets.empty()) {
                    LOG(INFO, "Server") << "Auto-updating " << auto_update_targets.size() << " model(s)..." << std::endl;
                    (void)model_manager_->sync_models(auto_update_targets, /*dry_run=*/false);
                }
            } catch (const std::exception& e) {
                LOG(WARNING, "Server") << "Model update check failed: " << e.what() << std::endl;
            } catch (...) {
                LOG(WARNING, "Server") << "Model update check failed with unknown error" << std::endl;
            }
        } else {
            LOG(DEBUG, "Server")
                << "Automatic model update checks are disabled" << std::endl;
        }

        update_check_done_ = true;
    });
}

void Server::run() {
    listener_->start();
}

void Server::stop() {
    std::lock_guard<std::mutex> lock(stop_mutex_);
    if (listener_->stop()) {
        LOG(INFO, "Server") << "Unloading models and stopping backend servers..." << std::endl;
        try {
            router_->unload_model();
        } catch (const std::exception& e) {
            LOG(ERROR, "Server") << "Error during cleanup: " << e.what() << std::endl;
        }

        LOG(INFO, "Server") << "Shutting down telemetry queue..." << std::endl;
        lemon::telemetry::shutdown();

        LOG(INFO, "Server") << "Cleanup complete" << std::endl;
    }

    model_manager_->cancel_sync();

    if (model_cache_warmup_thread_.joinable()) {
        model_cache_warmup_thread_.join();
    }

    model_manager_->wait_until_sync_idle();
}

double Server::get_cpu_usage() {
    return metrics_platform_->get_cpu_usage(cpu_stats_mutex_,
                                            last_cpu_stats_.total,
                                            last_cpu_stats_.total_idle);
}

bool Server::should_shutdown() const {
    return listener_->shutdown_requested();
}

void Server::set_shutdown_requested(bool requested) {
    listener_->set_shutdown_requested(requested);
}

bool Server::is_running() const {
    return listener_->is_running();
}

bool Server::startup_failed() const {
    return listener_->startup_failed();
}

void Server::handle_request(const httplib::Request& req, httplib::Response& res) {
    if (!registry_->dispatch(req, res)) {
        res.status = 404;
        res.set_content("{\"error\": \"Not Found. For API documentation, use /v1/docs.\"}", "application/json");
    };
    web_server.Get("/docs", docs_not_found);
    web_server.Get(R"(/docs/(.*))", docs_not_found);

    // Override default headers for static files to include no-cache
    // This ensures the web UI always gets the latest version
    web_server.set_file_request_handler([](const httplib::Request& req, httplib::Response& res) {
        // Add no-cache headers for static files
        res.set_header("Cache-Control", "no-cache, no-store, must-revalidate");
        res.set_header("Pragma", "no-cache");
        res.set_header("Expires", "0");
    });
}

void Server::setup_cors(httplib::Server &web_server) {
    // Set CORS headers for all responses
    web_server.set_default_headers({
        {"Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS"},
        {"Access-Control-Allow-Headers", "Content-Type, Authorization, X-Client-Session-Id, X-Account-Session-Id, mcp-protocol-version, traceparent, Mcp-Session-Id"}
    });

    // Handle preflight OPTIONS requests
    web_server.Options(".*", [](const httplib::Request&, httplib::Response& res) {
        res.status = 204;
    });

    // Catch-all error handler - must be last!
    web_server.set_error_handler([](const httplib::Request& req, httplib::Response& res) {
        LOG(ERROR, "Server") << "Error " << res.status << ": " << req.method << " " << req.path << std::endl;

        if (res.status == 404) {
            // Only set generic "endpoint not found" if no content was already set
            // This preserves specific error messages (e.g., "model not found")
            if (res.body.empty()) {
                nlohmann::json error = {
                    {"error", {
                        {"message", "The requested endpoint does not exist"},
                        {"type", "not_found"},
                        {"path", req.path}
                    }}
                };
                res.set_content(error.dump(), "application/json");
            }
        } else if (res.status == 400) {
            // Log more details about 400 errors
            LOG(ERROR, "Server") << "400 Bad Request details - Body length: " << req.body.length()
                      << ", Content-Type: " << req.get_header_value("Content-Type") << std::endl;
            // Ensure a response is sent
            if (res.body.empty()) {
                nlohmann::json error = {
                    {"error", {
                        {"message", "Bad request"},
                        {"type", "bad_request"}
                    }}
                };
                res.set_content(error.dump(), "application/json");
            }
        }
    });
}

std::string Server::resolve_host_to_ip(int ai_family, const std::string& host) {
    struct addrinfo hints = {0};
    hints.ai_family = ai_family;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = 0; // No AI_ADDRCONFIG: allows loopback resolution when offline

    struct addrinfo *result = nullptr;

    // Check return value (0 is success)
    if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0) {
        LOG(WARNING, "Server") << "resolution failed for " << host << " no " << (ai_family == AF_INET ? "IPv4" : ai_family == AF_INET6 ? "IPv6" : "") << " resolution found." << std::endl;
        return ""; // Return empty string on failure, don't return void
    }

    if (result == nullptr) return "";

    // Use INET6_ADDRSTRLEN to be safe for both (it's larger)
    char addrstr[INET6_ADDRSTRLEN];
    void *ptr = nullptr;

    // Safety Check - verify what we actually got back
    if (result->ai_family == AF_INET) {
        struct sockaddr_in *ipv4 = (struct sockaddr_in *)result->ai_addr;
        ptr = &(ipv4->sin_addr);
    } else if (result->ai_family == AF_INET6) {
        struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)result->ai_addr;
        ptr = &(ipv6->sin6_addr);
    } else {
        freeaddrinfo(result);
        return "";
    }

    // Convert binary IP to string
    inet_ntop(result->ai_family, ptr, addrstr, sizeof(addrstr));

    std::string resolved_ip(addrstr);
    freeaddrinfo(result);
    return resolved_ip;
}

void Server::setup_http_logger(httplib::Server &web_server) {
    // Add request logging for ALL requests (except health checks and stats endpoints)
    web_server.set_logger([this](const httplib::Request& req, const httplib::Response& res) {
        if (req.path == "/metrics") {
            if (res.status == 200) {
                bool expected = false;
                if (metrics_access_logged_.compare_exchange_strong(expected, true)) {
                    LOG(INFO, "Server") << req.method << " " << req.path << " - " << res.status << std::endl;
                }
            } else {
                LOG(WARNING, "Server") << req.method << " " << req.path << " - " << res.status << std::endl;
            }
            return;
        }

        // Skip logging health checks and stats endpoints to reduce log noise
        if (req.path == "/api/v0/health" || req.path == "/api/v1/health" ||
            req.path == "/v0/health" || req.path == "/v1/health" || req.path == "/live" ||
            is_quiet_polling_path(req.path)) {
            return;
        }

        // Determine if this is a high-signal request or noise (static assets, repeated queries)
        bool is_quiet_get = (req.method == "GET" && (
            req.path == "/" ||
            req.path == "/api/v0/models" || req.path == "/api/v1/models" ||
            req.path == "/v0/models" || req.path == "/v1/models" ||
            req.path == "/api/v0/system-info" || req.path == "/api/v1/system-info" ||
            req.path == "/v0/system-info" || req.path == "/v1/system-info" ||
            req.path == "/api/v0/system-checks" || req.path == "/api/v1/system-checks" ||
            req.path == "/v0/system-checks" || req.path == "/v1/system-checks" ||
            req.path.find(".js") != std::string::npos ||
            req.path.find(".css") != std::string::npos ||
            req.path.find(".svg") != std::string::npos ||
            req.path.find(".png") != std::string::npos ||
            req.path.find(".ico") != std::string::npos ||
            req.path.find(".woff") != std::string::npos
        ));

        if (!is_quiet_get) {
            LOG(DEBUG, "Server") << req.method << " " << req.path << " - " << res.status << std::endl;
        }
    });
}

void Server::run() {
    std::string host = config_->host();
    LOG(INFO, "Server") << "Starting HTTP server on " << host << ":" << port_ << std::endl;

    std::string ipv4 = resolve_host_to_ip(AF_INET, host);
    std::string ipv6 = resolve_host_to_ip(AF_INET6, host);

    LOG(INFO, "Server") << "Host resolution: IPv4=" << (ipv4.empty() ? "(none)" : ipv4)
                        << ", IPv6=" << (ipv6.empty() ? "(none)" : ipv6) << std::endl;

    if (ipv4.empty() && ipv6.empty()) {
        throw std::runtime_error("Failed to resolve host '" + host + "' to any address. "
                                 "Cannot start server.");
    }

    // Fail fast if the port is already taken (usually another lemond). Detecting
    // it here keeps the error from being buried under later startup logs.
    {
        std::string in_use_ip;
        if (!ipv4.empty() && utils::is_tcp_listener_active(AF_INET, ipv4, port_)) {
            in_use_ip = ipv4;
        } else if (!ipv6.empty() && utils::is_tcp_listener_active(AF_INET6, ipv6, port_)) {
            in_use_ip = ipv6;
        }
        if (!in_use_ip.empty()) {
            std::string msg = "Port " + std::to_string(port_) + " on " + in_use_ip +
                " is already in use. Another Lemonade server (lemond) is likely "
                "already running on this port. This instance will now exit.";
            std::cerr << "[Server] ERROR: " << msg << std::endl;  // terminal visibility
            LOG(ERROR, "Server") << msg << std::endl;
            startup_failed_ = true;
            return;
        }
    }

    // Operators binding beyond loopback should secure the server with an API
    // key, since every endpoint is reachable from other machines once the host
    // is non-loopback. The regular API routes (/api, /v0, /v1) are gated by
    // api_key_; the /internal/* control endpoints (shutdown, set, config) are
    // gated by admin_api_key_, which defaults to api_key_. Setting only
    // LEMONADE_ADMIN_API_KEY therefore protects /internal/* but still leaves the
    // inference and model-management endpoints exposed, so we warn unless the
    // regular key is set.
    auto warn_if_unsecured = [this](const std::string& bound_host,
                                    const std::string& v4, const std::string& v6) {
        auto is_loopback = [](const std::string& ip) {
            return ip.empty() || ip.rfind("127.", 0) == 0 || ip == "::1";
        };
        if (is_loopback(v4) && is_loopback(v6)) {
            return;
        }
        if (api_key_.empty() && admin_api_key_.empty()) {
            LOG(WARNING, "Server")
                << "Serving on non-loopback host '" << bound_host
                << "' without an API key. All endpoints, including the /internal/* "
                   "control endpoints, are reachable from other machines "
                   "unauthenticated. Set LEMONADE_API_KEY to secure all endpoints; "
                   "LEMONADE_ADMIN_API_KEY on its own only secures the /internal/* "
                   "control endpoints." << std::endl;
        } else if (api_key_.empty()) {
            LOG(WARNING, "Server")
                << "Serving on non-loopback host '" << bound_host
                << "' with only an admin API key set. The /internal/* control "
                   "endpoints are protected, but the inference and model-management "
                   "endpoints (/api, /v0, /v1) are reachable from other machines "
                   "unauthenticated. Set LEMONADE_API_KEY to secure them." << std::endl;
        }
    };
    warn_if_unsecured(host, ipv4, ipv6);

    running_ = true;

    // Start WebSocket server for realtime API and log streaming
    if (websocket_server_) {
        if (websocket_server_->start()) {
            LOG(INFO, "Server") << "WebSocket server started on port "
                                << websocket_server_->get_port() << std::endl;
        } else {
            LOG(WARNING, "Server") << "Failed to start WebSocket server" << std::endl;
        }
    }

    while (true) {
        // Check for shutdown signal from the main thread
        if (shutdown_requested_.load()) {
            LOG(INFO, "Server") << "Shutdown requested, stopping server..." << std::endl;
            stop();
            break;
        }

        const std::size_t listener_count =
            static_cast<std::size_t>(!ipv4.empty()) +
            static_cast<std::size_t>(!ipv6.empty());
        utils::ListenerStartupState listener_startup(listener_count);

        if (!ipv4.empty()) {
            // setup ipv4 thread
            setup_http_logger(*http_server_);
            http_v4_thread_ = std::thread([this, ipv4, &listener_startup]() {
                LOG(INFO, "Server") << "Binding IPv4 HTTP server to " << ipv4 << ":" << port_ << "..." << std::endl;
                int result = http_front_->bind_to_port(ipv4, port_);
                if (result <= 0) {
                    LOG(ERROR, "Server") << "Failed to bind IPv4 HTTP server to " << ipv4 << ":" << port_ << std::endl;
                    listener_startup.record_bind_failure();
                    return;
                }
                // The routed server's keep-alive loop runs only while it sees
                // a valid listen socket
                http_server_->set_listen_socket(http_front_->listen_socket());
                LOG(INFO, "Server") << "IPv4 HTTP server listening on " << ipv4 << ":" << port_ << std::endl;
                listener_startup.record_bind_success();
                if (!http_front_->listen_after_bind()) {
                    LOG(ERROR, "Server") << "IPv4 HTTP server listen_after_bind() failed" << std::endl;
                    listener_startup.record_listen_failure();
                }
            });
        }
        if (!ipv6.empty()) {
            // setup ipv6 thread
            setup_http_logger(*http_server_v6_);
            http_v6_thread_ = std::thread([this, ipv6, &listener_startup]() {
                LOG(INFO, "Server") << "Binding IPv6 HTTP server to [" << ipv6 << "]:" << port_ << "..." << std::endl;
                int result = http_front_v6_->bind_to_port(ipv6, port_);
                if (result <= 0) {
                    LOG(ERROR, "Server") << "Failed to bind IPv6 HTTP server to [" << ipv6 << "]:" << port_ << std::endl;
                    listener_startup.record_bind_failure();
                    return;
                }
                http_server_v6_->set_listen_socket(http_front_v6_->listen_socket());
                LOG(INFO, "Server") << "IPv6 HTTP server listening on [" << ipv6 << "]:" << port_ << std::endl;
                listener_startup.record_bind_success();
                if (!http_front_v6_->listen_after_bind()) {
                    LOG(ERROR, "Server") << "IPv6 HTTP server listen_after_bind() failed" << std::endl;
                    listener_startup.record_listen_failure();
                }
            });
        }

        while (!listener_startup.bind_attempts_complete() &&
               !shutdown_requested_.load() && !rebind_requested_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        if (listener_startup.failed()) {
            LOG(ERROR, "Server") << "Could not start HTTP listeners for every address resolved for host '"
                                 << host << "' (bind or listen failure). Server startup aborted." << std::endl;
            stop();
            if (http_v4_thread_.joinable())
                http_v4_thread_.join();
            if (http_v6_thread_.joinable())
                http_v6_thread_.join();
            startup_failed_ = true;
            break;
        }

        // Enumerate all RFC1918 interfaces to determine if we can broadcast.
        // The beacon will send per-interface with the correct IP in the payload.
        auto rfc1918Interfaces = udp_beacon_.getLocalRFC1918Interfaces();
        bool bcast = config_->broadcast();
        if (!rfc1918Interfaces.empty() && bcast) {
            std::cout << "[Server] [Net Broadcast] Broadcasting on " << rfc1918Interfaces.size()
                      << " RFC1918 interface(s):";
            for (const auto& iface : rfc1918Interfaces) {
                std::cout << " " << iface.ipAddress << " (bcast " << iface.broadcastAddress << ")";
            }
            std::cout << std::endl;
            udp_beacon_.startBroadcasting(
                13305, // Broadcast port best to not make it adjustable, so clients dont have to scan.
                port_,
                2
            );
        } else if (!rfc1918Interfaces.empty() && !bcast) {
            LOG(INFO, "Server") << "Broadcasting disabled by configuration or CLI option" << std::endl;
        } else {
            LOG(INFO, "Server") << "Unable to broadcast my existance please use a RFC1918 IPv4," << std::endl
                        << "or hostname that resolves to RFC1918 IPv4." << std::endl;
        }

        // Wait for listener threads, but check periodically for shutdown or rebind signals.
        // The threads are blocked in listen_after_bind(), which only returns when
        // the server is stopped or an error occurs.
        while ((http_v4_thread_.joinable() || http_v6_thread_.joinable()) &&
               !shutdown_requested_.load() && !rebind_requested_.load() &&
               !listener_startup.failed()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        // If shutdown was requested while the server was running, stop it now
        // to unblock the listener threads (they're stuck in listen_after_bind)
        if (shutdown_requested_.load()) {
            LOG(INFO, "Server") << "Shutdown requested, stopping server..." << std::endl;
            stop();
            // Join the threads (they should exit quickly after stop() is called)
            if (http_v4_thread_.joinable())
                http_v4_thread_.join();
            if (http_v6_thread_.joinable())
                http_v6_thread_.join();
            break;  // Exit the main loop
        }

        if (listener_startup.failed()) {
            LOG(ERROR, "Server") << "An HTTP listener stopped unexpectedly. Server exiting." << std::endl;
            stop();
            if (http_v4_thread_.joinable())
                http_v4_thread_.join();
            if (http_v6_thread_.joinable())
                http_v6_thread_.join();
            startup_failed_ = true;
            break;
        }

        // If rebind was requested, stop() has already been called by apply_config_side_effects().
        // Just join the threads so they can be restarted with new settings.
        if (rebind_requested_.load()) {
            // Wait for threads to finish (stop() was already called)
            if (http_v4_thread_.joinable())
                http_v4_thread_.join();
            if (http_v6_thread_.joinable())
                http_v6_thread_.join();
            // Continue to rebind logic below (don't break)
        } else {
            // Normal path: threads exited naturally (no shutdown, no rebind)
            // Join the threads
            if (http_v4_thread_.joinable())
                http_v4_thread_.join();
            if (http_v6_thread_.joinable())
                http_v6_thread_.join();
        }

        if (!rebind_requested_) {
            break;  // Normal exit (stop() was called)
        }

        // Rebind requested: re-resolve host, recreate HTTP servers, loop back to bind+listen
        host = config_->host();
        ipv4 = resolve_host_to_ip(AF_INET, host);
        ipv6 = resolve_host_to_ip(AF_INET6, host);
        warn_if_unsecured(host, ipv4, ipv6);
        LOG(INFO, "Server") << "Rebinding to " << host << ":" << port_ << "..." << std::endl;
        rebind_requested_ = false;
        setup_http_servers();
    }
}


bool Server::should_shutdown() const {
    return shutdown_requested_.load();
}

void Server::set_shutdown_requested(bool requested) {
    shutdown_requested_.store(requested);
}

bool Server::is_running() const {
    return running_;
}

bool Server::startup_failed() const {
    return startup_failed_;
}

void Server::stop() {
    if (running_) {
        LOG(INFO, "Server") << "Stopping HTTP server..." << std::endl;
        udp_beacon_.stopBroadcasting();
        stop_http_listeners();
        running_ = false;
        shutdown_requested_ = false;  // Reset for potential future use

        if (websocket_server_) {
            LOG(INFO, "Server") << "Stopping WebSocket server..." << std::endl;
            websocket_server_->stop();
        }

        if (router_) {
            LOG(INFO, "Server") << "Unloading models and stopping backend servers..." << std::endl;
            try {
                router_->unload_model();
            } catch (const std::exception& e) {
                LOG(ERROR, "Server") << "Error during cleanup: " << e.what() << std::endl;
            }
        }

        LOG(INFO, "Server") << "Shutting down telemetry queue..." << std::endl;
        lemon::telemetry::shutdown();

        LOG(INFO, "Server") << "Cleanup complete" << std::endl;
    }

    if (model_manager_) {
        model_manager_->cancel_sync();
    }

    if (model_cache_warmup_thread_.joinable()) {
        model_cache_warmup_thread_.join();
    }

    {
        std::lock_guard<std::mutex> lock(background_sync_mutex_);
        for (auto& task : background_sync_threads_) {
            if (task.thread.joinable()) {
                task.thread.join();
            }
        }
        background_sync_threads_.clear();
    }
}

// Generates an actionable error message for model loading failures.
// Handles three cases:
//   1. Model exists but was filtered out (e.g., NPU model on non-NPU system)
//   2. Model doesn't exist in the registry at all
//   3. Model exists but failed to load (engine error)
nlohmann::json Server::create_model_error(const std::string& requested_model, const std::string& exception_msg) {
    nlohmann::json error_response;

    // Case 1: Check if this model exists but was filtered out due to system requirements
    std::string filter_reason = model_manager_->get_model_filter_reason(requested_model);

    if (!filter_reason.empty()) {
        // Model exists but is not available on this system
        std::string message = "Model '" + requested_model + "' is not available on this system. " + filter_reason;

        error_response["error"] = {
            {"message", message},
            {"type", "model_not_supported"},
            {"param", "model"},
            {"code", "model_not_supported"},
            {"requested_model", requested_model}
        };

        return error_response;
    }

    // Case 2: Check if model doesn't exist in the registry at all
    if (!model_manager_->model_exists(requested_model)) {
        std::string message = "Model '" + requested_model + "' was not found. ";

        // Get available models and suggest some
        auto available_models = model_manager_->get_supported_models();

        if (!available_models.empty()) {
            // Collect model names
            std::vector<std::string> model_names;
            model_names.reserve(available_models.size());
            for (const auto& [name, info] : available_models) {
                model_names.push_back(name);
            }

            // Sort alphabetically for consistent output
            std::sort(model_names.begin(), model_names.end());

            // Show up to 3 available models
            const size_t max_suggestions = 3;
            size_t count = std::min(model_names.size(), max_suggestions);

            message += "Available models include: ";
            for (size_t i = 0; i < count; ++i) {
                if (i > 0) message += ", ";
                message += "'" + model_names[i] + "'";
            }

            if (model_names.size() > max_suggestions) {
                message += ", and " + std::to_string(model_names.size() - max_suggestions) + " more";
            }
            message += ". ";
        }

        message += "Use 'lemonade list' or GET /api/v1/models?show_all=true to see all available models.";

        // Add FLM hint for -FLM model names when FLM is not ready
        if (requested_model.size() > 4 &&
            requested_model.substr(requested_model.size() - 4) == "-FLM") {
            auto flm_status = SystemInfoCache::get_flm_status();
            if (!flm_status.is_ready()) {
                message += " The FLM backend is not ready: " + flm_status.message + ".";
                if (!flm_status.action.empty()) {
                    message += " " + flm_status.action + ".";
                }
            }
        }

        error_response["error"] = {
            {"message", message},
            {"type", "model_not_found"},
            {"param", "model"},
            {"code", "model_not_found"},
            {"requested_model", requested_model}
        };

        return error_response;
    }

    // Case 3: Model exists and is available, but failed to load.
    if (exception_msg.rfind("Routing residency conflict:", 0) == 0) {
        error_response["error"] = {
            {"message", exception_msg},
            {"type", "router_residency_conflict"},
            {"param", "model"},
            {"code", "router_residency_conflict"},
            {"requested_model", requested_model}
        };
        return error_response;
    }

    if (exception_msg.find("are pinned") != std::string::npos) {
        error_response["error"] = {
            {"message", exception_msg},
            {"type", "slots_pinned_error"},
            {"param", "model"},
            {"code", "slots_pinned_error"},
            {"requested_model", requested_model}
        };
        return error_response;
    }

    // Return the actual exception message so the user knows what went wrong
    std::string message = "Failed to load model '" + requested_model + "': " + exception_msg;

    error_response["error"] = {
        {"message", message},
        {"type", "model_load_error"},
        {"param", "model"},
        {"code", "model_load_error"},
        {"requested_model", requested_model}
    };

    return error_response;
}

// This function is called by:
//   - handle_chat_completions() - /chat/completions endpoint
//   - handle_completions() - /completions endpoint
//   - handle_load() - /load endpoint
//
// Behavior:
//   1. If model is already loaded: Return immediately (no-op)
//   2. If model is not downloaded: Download it (first-time use)
//   3. If model is downloaded: Use cached version
//      (do not check the remote registry for updates)
//
// Note: Only the /pull endpoint checks the model's recorded registry for
// updates (do_not_upgrade=false).

// Load-level options that may be forwarded to RecipeOptions during auto-load.
// Keep this an explicit allowlist so request-scoped fields do not leak into
// recipe options.
nlohmann::json Server::extract_auto_load_options(const json& request) {
    nlohmann::json result = json::object();

    auto extract_if_present =
        [&request, &result](const std::string& key) {
            if (request.contains(key)) {
                result[key] = request[key];
            }
        };

    extract_if_present("ctx_size");

    return result;
}

void Server::auto_load_model_if_needed(
    const std::string& requested_model,
    const json& request_options,
    LoadPurpose load_purpose) {
    // A live process follows its current use without a reload: routing work
    // promotes it to RoutingHelper, while direct inference demotes it into the
    // counted Standard pool. Destination-pool admission remains authoritative.
    if (router_->ensure_loaded_model_residency(
            requested_model, load_purpose)) {
        LOG(DEBUG, "Server")
            << "Model already loaded: " << requested_model
            << " (residency="
            << residency_class_to_string(
                   residency_class_for_load_purpose(load_purpose))
            << ")" << std::endl;
        if (request_options.contains("ctx_size")) {
            auto loaded_ctx = router_->get_model_recipe_options(requested_model)
                                  .get_option("ctx_size");
            LOG(DEBUG, "Server")
                << "Ignoring requested ctx_size=" << request_options["ctx_size"]
                << " for already-loaded " << requested_model
                << " (loaded ctx_size=" << loaded_ctx << ")" << std::endl;
        }
        return;
    }

    // Log the auto-loading action
    LOG(INFO, "Server") << "Auto-loading model: " << requested_model << std::endl;

    // Get model info
    if (!model_manager_->model_exists(requested_model)) {
        throw std::runtime_error("Model not found: " + requested_model);
    }

    auto info = model_manager_->get_model_info(requested_model);

    // Collections have no backend of their own — load each component instead.
    if (is_omni_collection_recipe(info.recipe)) {
        ensure_collection_loaded(info);
        return;
    }

    // Download model if not cached (first-time use)
    // IMPORTANT: Use do_not_upgrade=true to prevent checking the remote registry for updates
    // This means:
    //   - If model is NOT downloaded: Download it from its recorded registry
    //   - If model IS downloaded: Skip the registry API check entirely (use cached version)
    // Only the /pull endpoint should check for updates (uses do_not_upgrade=false)
    if (!model_manager_->backend_self_manages_downloads(info.recipe) &&
        !model_manager_->is_model_downloaded(requested_model)) {
        LOG(INFO, "Server") << "Model not cached, downloading from "
                            << remote_registry_display_name(
                                   parse_remote_registry_source(info.registry_source))
                            << "..." << std::endl;
        LOG(INFO, "Server") << "This may take several minutes for large models." << std::endl;
        model_manager_->download_registered_model(info, true);
        LOG(INFO, "Server") << "Model download complete: " << requested_model << std::endl;

        // CRITICAL: Refresh model info after download to get correct resolved_path
        // The resolved_path is computed based on filesystem, so we need fresh info now that files exist
        info = model_manager_->get_model_info(requested_model);
    }

    // Load model with do_not_upgrade=true, applying per-request options on first load.
    // For FLM models: FastFlowLMServer will handle download internally if needed
    // For non-FLM models: Model should already be cached at this point
    router_->load_model(requested_model, info,
                        RecipeOptions(info.recipe, request_options), true,
                        /*allow_reload_on_option_change=*/false,
                        /*pinned=*/std::nullopt,
                        load_purpose);
    LOG(INFO, "Server") << "Model loaded successfully: " << requested_model << std::endl;
}

void Server::ensure_collection_loaded(const ModelInfo& info) {
    LOG(INFO, "Server") << "Loading collection components for: " << info.model_name << std::endl;
    for (const auto& component : info.components) {
        if (!model_manager_->model_exists(component)) {
            LOG(WARNING, "Server") << "Skipping unknown component: " << component << std::endl;
            continue;
        }
        if (router_->is_model_loaded(component)) {
            LOG(DEBUG, "Server") << "Component already loaded: " << component << std::endl;
            continue;
        }
        auto comp_info = model_manager_->get_model_info(component);
        if (!comp_info.downloaded) {
            LOG(INFO, "Server") << "Downloading component: " << component << std::endl;
            model_manager_->download_registered_model(comp_info);
            comp_info = model_manager_->get_model_info(component);
        }
        LOG(INFO, "Server") << "Loading component: " << component << std::endl;
        // The collection load request does not become the component request
        // layer. Router already reads the component's saved options from comp_info,
        // so an empty request preserves normal model/architecture/backend scope
        // semantics.
        router_->load_model(
            component, comp_info, RecipeOptions(comp_info.recipe, json::object()), true,
            /*allow_reload_on_option_change=*/true);
    }
}

void Server::handle_health(const httplib::Request& req, httplib::Response& res) {
    // For HEAD requests, just return 200 OK without processing
    if (req.method == "HEAD") {
        res.status = 200;
        return;
    }

    nlohmann::json response = {{"status", "ok"}};

    // Add version information
    response["version"] = LEMON_VERSION_STRING;

    // Add telemetry state using in-memory runtime configuration
    if (config_) {
        nlohmann::json telemetry_info = {
            {"enabled", config_->telemetry_enabled()}
        };
        if (config_->telemetry_enabled()) {
            std::vector<std::string> captures;
            if (!config_->telemetry_hide_inputs()) {
                captures.push_back("inputs");
            }
            if (!config_->telemetry_hide_outputs()) {
                captures.push_back("outputs");
            }
            if (!config_->telemetry_hide_thinking()) {
                captures.push_back("thinking");
            }
            telemetry_info["captures"] = captures;
        }
        response["telemetry"] = telemetry_info;
    }

    // Add model loaded information like Python implementation
    std::string loaded_model = router_->get_loaded_model();

    response["model_loaded"] = loaded_model.empty() ? nlohmann::json(nullptr) : nlohmann::json(loaded_model);

    // Multi-model support: Add all loaded models
    response["all_models_loaded"] = router_->get_all_loaded_models();

    // Add max model limits
    response["max_models"] = router_->get_max_model_limits();

    // Add pinned model counts
    response["pinned_models"] = router_->get_pinned_model_counts();
    response["pinned_helper_models"] = router_->get_pinned_helper_counts();

    // Add WebSocket server port for realtime API and log streaming
    if (websocket_server_ && websocket_server_->is_running()) {
        response["websocket_port"] = websocket_server_->get_port();
    }

    // Add update check status
    response["update_check_done"] = update_check_done_.load();

    res.set_content(response.dump(), "application/json");
}

void Server::handle_live(const httplib::Request& req, httplib::Response& res) {
    // For HEAD requests, just return 200 OK
    if (req.method == "HEAD") {
        res.status = 200;
        return;
    }

    // liveness response
    static const char* kLiveResponse = R"({"status":"ok"})";

    res.set_content(kLiveResponse, "application/json");
    res.status = 200;
}

void Server::handle_docs(const httplib::Request& req, httplib::Response& res) {
    std::string docs_dir = utils::get_resource_path("resources/docs");

    // Echo back the prefix the client used so the URLs stay valid on all four.
    std::string prefix = req.path.substr(0, req.path.size() - std::string("/docs").size());

    nlohmann::json docs = nlohmann::json::array();
    for (const ApiDoc& doc : list_api_docs(docs_dir)) {
        docs.push_back({
            {"id", doc.id},
            {"title", doc.title},
            {"url", prefix + "/docs/" + doc.id},
            {"bytes", doc.bytes}
        });
    }

    nlohmann::json body = {
        {"version", LEMON_VERSION_STRING},
        {"format", "text/markdown"},
        {"docs", docs}
    };
    res.set_content(body.dump(2), "application/json");
    res.status = 200;
}

void Server::handle_doc_page(const httplib::Request& req, httplib::Response& res) {
    std::string docs_dir = utils::get_resource_path("resources/docs");

    std::string content;
    if (!read_api_doc(docs_dir, req.matches[1].str(), content)) {
        res.status = 404;
        res.set_content("{\"error\": \"Documentation page not found\"}", "application/json");
        return;
    }

    res.set_content(content, "text/markdown");
    res.status = 200;
}

void Server::handle_model_update_check(const httplib::Request& req, httplib::Response& res) {
    (void)req;

    // A manual check is intentionally independent from
    // auto_check_model_updates, but full offline mode remains authoritative.
    if (config_->offline()) {
        res.status = 409;
        res.set_content(
            nlohmann::json{{"error", "Cannot check model updates while offline=true"}}.dump(),
            "application/json");
        return;
    }

    try {
        auto check_res = model_manager_->check_for_model_updates();

        nlohmann::json response = {
            {"status", check_res.failed_models.empty() ? "success" : "failed"},
            {"updates_available", check_res.updated_models.size()},
            {"models", check_res.updated_models},
            {"failed_models", check_res.failed_models}
        };

        res.set_content(response.dump(), "application/json");
    } catch (const std::exception& e) {
        LOG(WARNING, "Server") << "Manual model update check failed: " << e.what() << std::endl;
        res.status = 500;
        res.set_content(
            nlohmann::json{{"error", std::string("Model update check failed: ") + e.what()}}.dump(),
            "application/json");
    }
}

void Server::handle_models_sync(const httplib::Request& req, httplib::Response& res) {
    if (config_->offline()) {
        res.status = 409;
        res.set_content(
            nlohmann::json{{"error", "Cannot sync model updates while offline=true"}}.dump(),
            "application/json");
        return;
    }

    std::vector<std::string> target_models;
    bool dry_run = false;
    bool async_dispatch = true;
    bool async_specified = false;
    bool attach_if_running = false;

    if (!req.body.empty()) {
        try {
            auto body_json = nlohmann::json::parse(req.body);
            if (!body_json.is_object()) {
                res.status = 400;
                res.set_content(
                    nlohmann::json{{"error", "Request body must be a JSON object"}}.dump(),
                    "application/json");
                return;
            }
            if (body_json.contains("models")) {
                if (!body_json["models"].is_array()) {
                    res.status = 400;
                    res.set_content(
                        nlohmann::json{{"error", "'models' must be an array of strings"}}.dump(),
                        "application/json");
                    return;
                }
                for (const auto& item : body_json["models"]) {
                    if (!item.is_string()) {
                        res.status = 400;
                        res.set_content(
                            nlohmann::json{{"error", "Each item in 'models' must be a string"}}.dump(),
                            "application/json");
                        return;
                    }
                    target_models.push_back(item.get<std::string>());
                }
            }
            if (body_json.contains("dry_run")) {
                if (!body_json["dry_run"].is_boolean()) {
                    res.status = 400;
                    res.set_content(
                        nlohmann::json{{"error", "'dry_run' must be a boolean"}}.dump(),
                        "application/json");
                    return;
                }
                dry_run = body_json["dry_run"].get<bool>();
            }
            if (body_json.contains("async")) {
                if (!body_json["async"].is_boolean()) {
                    res.status = 400;
                    res.set_content(
                        nlohmann::json{{"error", "'async' must be a boolean"}}.dump(),
                        "application/json");
                    return;
                }
                async_dispatch = body_json["async"].get<bool>();
                async_specified = true;
            }
            if (body_json.contains("attach_if_running")) {
                if (!body_json["attach_if_running"].is_boolean()) {
                    res.status = 400;
                    res.set_content(
                        nlohmann::json{{"error", "'attach_if_running' must be a boolean"}}.dump(),
                        "application/json");
                    return;
                }
                attach_if_running = body_json["attach_if_running"].get<bool>();
            }
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(
                nlohmann::json{{"error", std::string("Invalid JSON body: ") + e.what()}}.dump(),
                "application/json");
            return;
        }
    }

    if (dry_run) {
        try {
            nlohmann::json sync_result = model_manager_->sync_models(target_models, true);
            res.set_content(sync_result.dump(), "application/json");
            res.status = 200;
        } catch (const std::exception& e) {
            LOG(WARNING, "Server") << "Model sync dry-run failed: " << e.what() << std::endl;
            res.status = 500;
            res.set_content(
                nlohmann::json{{"error", std::string("Model sync dry-run failed: ") + e.what()}}.dump(),
                "application/json");
        }
        return;
    }

    if (!async_specified || async_dispatch) {
        auto enqueue_res = model_manager_->enqueue_sync(target_models, attach_if_running);
        if (enqueue_res.already_running) {
            nlohmann::json status = model_manager_->get_sync_status(enqueue_res.sync_id);
            res.set_content(status.dump(), "application/json");
            res.status = 200;
            return;
        }

        std::lock_guard<std::mutex> lock(background_sync_mutex_);

        for (auto it = background_sync_threads_.begin(); it != background_sync_threads_.end(); ) {
            if (it->finished->load() && it->thread.joinable()) {
                it->thread.join();
                it = background_sync_threads_.erase(it);
            } else {
                ++it;
            }
        }

        auto finished_flag = std::make_shared<std::atomic<bool>>(false);
        std::thread worker([this, finished_flag]() {
            try {
                LOG(INFO, "Server") << "Background model sync thread started" << std::endl;
                model_manager_->execute_sync();
                LOG(INFO, "Server") << "Background model sync thread completed successfully" << std::endl;
            } catch (const std::exception& e) {
                LOG(WARNING, "Server") << "Background model sync failed: " << e.what() << std::endl;
            }
            finished_flag->store(true);
        });

        background_sync_threads_.push_back({std::move(worker), finished_flag});

        nlohmann::json dispatched_result = model_manager_->get_sync_status(enqueue_res.sync_id);
        dispatched_result["message"] = "Model synchronization dispatched in background";
        dispatched_result["async"] = true;
        dispatched_result["already_in_progress"] = false;
        res.set_content(dispatched_result.dump(), "application/json");
        res.status = 202;
        return;
    }


    try {
        nlohmann::json sync_result = model_manager_->sync_models(target_models, false, attach_if_running);
        res.set_content(sync_result.dump(), "application/json");
        res.status = 200;
    } catch (const std::exception& e) {
        LOG(WARNING, "Server") << "Model sync failed: " << e.what() << std::endl;
        res.status = 500;
        res.set_content(
            nlohmann::json{{"error", std::string("Model sync failed: ") + e.what()}}.dump(),
            "application/json");
    }
}

void Server::handle_models_sync_status(const httplib::Request& req, httplib::Response& res) {
    try {
        uint64_t sync_id = 0;
        if (req.has_param("sync_id")) {
            try {
                sync_id = std::stoull(req.get_param_value("sync_id"));
            } catch (...) {
                res.status = 400;
                res.set_content(
                    nlohmann::json{{"error", "Invalid sync_id parameter"}}.dump(),
                    "application/json");
                return;
            }
        }
        nlohmann::json status = model_manager_->get_sync_status(sync_id);
        res.set_content(status.dump(), "application/json");
        res.status = 200;
    } catch (const std::exception& e) {
        LOG(WARNING, "Server") << "Failed to get sync status: " << e.what() << std::endl;
        res.status = 500;
        res.set_content(
            nlohmann::json{{"error", std::string("Failed to get sync status: ") + e.what()}}.dump(),
            "application/json");
    }
}

void Server::handle_models(const httplib::Request& req, httplib::Response& res) {
    // For HEAD requests, just return 200 OK without processing
    if (req.method == "HEAD") {
        res.status = 200;
        return;
    }

    // Check if we should show all models (for CLI list command) or only downloaded (OpenAI API behavior)
    bool show_all = req.has_param("show_all") && req.get_param_value("show_all") == "true";

    // OPTIMIZATION: For OpenAI API mode, use get_downloaded_models() which filters first
    // Only use get_supported_models() when we need to show ALL models
    std::map<std::string, ModelInfo> models;
    if (show_all) {
        models = model_manager_->get_supported_models();
    } else {
        models = model_manager_->get_downloaded_models();
    }

    nlohmann::json response;
    response["data"] = nlohmann::json::array();
    response["object"] = "list";

    for (const auto& [model_id, model_info] : models) {
        response["data"].push_back(model_info_to_json(model_id, model_info));
    }

    if (alias_manager_) {
        auto all_aliases = alias_manager_->get_all_aliases();
        for (const auto& [alias_id, target_name] : all_aliases) {
            if (models.count(alias_id) > 0) {
                continue;
            }
            std::string ultimate_target = target_name;
            if (auto resolved = alias_manager_->resolve_alias(alias_id)) {
                ultimate_target = *resolved;
            }
            std::string canonical_target = model_manager_->resolve_model_name(ultimate_target);
            if (models.count(canonical_target) > 0) {
                response["data"].push_back(model_info_to_json(alias_id, models.at(canonical_target)));
            } else if (models.count(ultimate_target) > 0) {
                response["data"].push_back(model_info_to_json(alias_id, models.at(ultimate_target)));
            }
        }
    }

    res.set_content(response.dump(), "application/json");
}

void Server::validate_model_registration_name(const std::string& model_name,
                                             bool require_user_namespace) {
    if (lemon::is_reserved_registration_name(model_name)) {
        throw std::invalid_argument(
            "Model names with 'extra.' / 'builtin.' prefixes are reserved, "
            "including as bare-name parts of a 'user.' alias. Received: " + model_name);
    }

    if (require_user_namespace &&
        (model_name.size() <= 5 || model_name.rfind("user.", 0) != 0)) {
        throw std::invalid_argument(
            "Registered model definitions must use a non-empty `user.*` name, "
            "for example `user.Phi-4-Mini-GGUF`. Received: " + model_name);
    }
}

void Server::normalize_model_registration_source(nlohmann::json& request_json,
                                                 bool local_import) {
    if (!request_json.contains("source") && !request_json.contains("registry_source")) {
        return;
    }

    std::optional<std::string> normalized_registry;
    if (request_json.contains("registry_source")) {
        if (!request_json["registry_source"].is_string()) {
            throw std::invalid_argument("`registry_source` must be a string when provided");
        }
        normalized_registry = remote_registry_source_name(
            parse_remote_registry_source(
                request_json["registry_source"].get<std::string>()));
    }

    if (request_json.contains("source")) {
        if (!request_json["source"].is_string()) {
            throw std::invalid_argument("`source` must be a string when provided");
        }
        const std::string public_source = request_json["source"].get<std::string>();
        if (is_remote_registry_source(public_source)) {
            const std::string normalized_public = remote_registry_source_name(
                parse_remote_registry_source(public_source));
            if (normalized_registry && *normalized_registry != normalized_public) {
                throw std::invalid_argument(
                    "`source` and `registry_source` must identify the same registry");
            }
            normalized_registry = normalized_public;
            request_json["source"] = normalized_public;
        } else if (!local_import && public_source != "local_upload" &&
                   public_source != "local_path" &&
                   public_source != "extra_models_dir") {
            throw std::invalid_argument(
                "Unsupported model source '" + public_source +
                "' (expected 'huggingface', 'modelscope', or a local source)");
        }
    }

    if (normalized_registry) {
        if (!request_json.contains("source") ||
            is_remote_registry_source(request_json["source"].get<std::string>())) {
            request_json["source"] = *normalized_registry;
        }
        request_json["registry_source"] = *normalized_registry;
    }
}

void Server::validate_and_canonicalize_collection_registration(
        const std::string& model_name,
        nlohmann::json& request_json,
        bool allow_embedded_models) {
    const std::string recipe = request_json.value("recipe", std::string());
    if (!is_model_collection_recipe(recipe)) {
        return;
    }

    if (request_json.contains("models") && !allow_embedded_models) {
        throw std::invalid_argument(
            "`models` embeds additional model definitions and is not accepted by "
            "the single-model registration endpoint; register components first");
    }

    if (request_json.contains("components")) {
        if (!request_json["components"].is_array()) {
            throw std::invalid_argument("`components` must be an array when provided");
        }
        for (const auto& component : request_json["components"]) {
            if (!component.is_string()) {
                throw std::invalid_argument(
                    "Every collection component must be a string model name");
            }
        }
    }

    if (auto err = model_manager_->validate_collection_request(model_name, request_json)) {
        throw std::invalid_argument(*err);
    }

    // Single-definition collections reference models already present in the
    // registry. Exported bundles keep their raw component names because their
    // embedded definitions are resolved by the collection import/download path.
    if (request_json.contains("components") && request_json["components"].is_array() &&
        (!request_json.contains("models") || !request_json["models"].is_array())) {
        for (auto& component : request_json["components"]) {
            component = model_manager_->resolve_model_name(component.get<std::string>());
        }
    }
}

std::string Server::register_model_definition_internal(
        const std::string& model_name,
        nlohmann::json& request_json,
        bool require_definition,
        bool allow_embedded_models,
        bool local_import) {
    if (!request_json.is_object()) {
        throw std::invalid_argument("Request body must be a JSON object");
    }

    if (request_json.contains("recipe") && !request_json["recipe"].is_string()) {
        throw std::invalid_argument("`recipe` must be a string when provided");
    }
    const std::string recipe = request_json.value("recipe", std::string());
    if (require_definition && recipe.empty()) {
        throw std::invalid_argument("A non-empty string `recipe` is required");
    }

    if (request_json.contains("checkpoint") && !request_json["checkpoint"].is_string()) {
        throw std::invalid_argument("`checkpoint` must be a string when provided");
    }
    if (request_json.contains("checkpoints")) {
        const auto& checkpoints = request_json["checkpoints"];
        if (!checkpoints.is_object() || !checkpoints.contains("main")) {
            throw std::invalid_argument(
                "If present, `checkpoints` must be an object containing `main`");
        }
        for (const auto& [role, checkpoint] : checkpoints.items()) {
            if (!checkpoint.is_string()) {
                throw std::invalid_argument(
                    "Every `checkpoints` value must be a string; invalid role: " + role);
            }
        }
    }

    const bool has_definition =
        require_definition || !recipe.empty() || request_json.contains("checkpoint") ||
        request_json.contains("checkpoints") || request_json.contains("components") ||
        request_json.contains("models");
    validate_model_registration_name(model_name, has_definition);

    if (request_json.contains("models") && !allow_embedded_models) {
        throw std::invalid_argument(
            "`models` embeds additional model definitions and is not accepted by "
            "the single-model registration endpoint; register components first");
    }

    normalize_model_registration_source(request_json, local_import);
    validate_and_canonicalize_collection_registration(
        model_name, request_json, allow_embedded_models);


    if (!has_definition && !local_import) {
        return model_name;
    }

    if (local_import) {
        std::string hf_cache = model_manager_->get_hf_cache_dir();
        std::string model_name_clean = model_name.substr(5);
        std::replace(model_name_clean.begin(), model_name_clean.end(), '/', '-');
        std::string dest_path = hf_cache + "/models--" + model_name_clean;

        LOG(INFO, "Server") << "Local import mode - resolving files in: "
                            << dest_path << std::endl;
        resolve_and_register_local_model(dest_path, model_name, request_json, hf_cache);
    } else {
        model_manager_->register_model(
            model_name,
            request_json,
            /*allow_missing_checkpoint=*/require_definition,
            /*replace_existing=*/require_definition);
    }

    return model_manager_->get_public_model_name(model_name);
}

void Server::handle_model_register(const httplib::Request& req, httplib::Response& res) {
    auto bad_request = [&res](const std::string& message) {
        res.status = 400;
        res.set_content(nlohmann::json{{"error", message}}.dump(), "application/json");
    };

    nlohmann::json request_json;
    if (!parse_required_json_body(req, res, request_json)) return;

    try {
        if (!request_json.is_object()) {
            bad_request("Request body must be a JSON object");
            return;
        }
        if (!request_json.contains("model_name") || !request_json["model_name"].is_string()) {
            bad_request("A string `model_name` is required");
            return;
        }

        const std::string model_name = request_json["model_name"].get<std::string>();
        const std::string public_name = register_model_definition_internal(
            model_name,
            request_json,
            /*require_definition=*/true,
            /*allow_embedded_models=*/false,
            /*local_import=*/false);

        nlohmann::json response = {
            {"status", "success"},
            {"model_name", public_name},
            {"canonical_model_name", model_name},
        };

        const auto models = model_manager_->get_supported_models();
        auto model_it = models.find(public_name);
        if (model_it != models.end()) {
            response["model"] = model_info_to_json(public_name, model_it->second);
        }

        res.set_content(response.dump(), "application/json");
    } catch (const std::invalid_argument& e) {
        bad_request(e.what());
    } catch (const nlohmann::json::exception& e) {
        bad_request(e.what());
    } catch (const std::exception& e) {
        LOG(ERROR, "Server") << "ERROR in handle_model_register: " << e.what() << std::endl;
        res.status = 500;
        res.set_content(nlohmann::json{{"error", e.what()}}.dump(), "application/json");
    }
}

int64_t Server::resolve_context_length(const std::string& model_id, const ModelInfo& info) const {
    // ctx_size stores -1 for "size this automatically", so only a positive
    // value answers; anything else falls through to the next source.
    auto ctx_size_of = [](const RecipeOptions& options) -> int64_t {
        const nlohmann::json ctx_json = options.get_option("ctx_size");
        return ctx_json.is_number() ? ctx_json.get<int64_t>() : 0;
    };

    if (router_) {
        const int64_t loaded_ctx =
            ctx_size_of(router_->get_model_recipe_options(resolve_alias_target(model_id)));
        if (loaded_ctx > 0) {
            return loaded_ctx;
        }

        const RecipeOptions no_request_options(info.recipe, nlohmann::json::object());
        const int64_t configured_ctx =
            ctx_size_of(router_->resolve_effective_options(info, no_request_options));
        if (configured_ctx > 0) {
            return configured_ctx;
        }
    }

    return info.max_context_window > 0 ? info.max_context_window : 0;
}

// Maximum collection-component nesting depth embedded in "models" arrays.
// Collection components are normally leaf models, but nothing prevents
// registering a collection as a component of another collection — including
// cyclically — so the embedding recursion must be bounded.
static constexpr int kMaxCollectionEmbedDepth = 3;

nlohmann::json Server::model_info_to_json(const std::string& model_id, const ModelInfo& info,
                                          int depth) {
    std::vector<std::string> public_components;
    public_components.reserve(info.components.size());
    for (const auto& component : info.components) {
        public_components.push_back(model_manager_->get_public_model_name(component));
    }
    nlohmann::json model_json = {
        {"id", model_id},
        {"object", "model"},
        {"created", 1234567890},
        {"owned_by", "lemonade"},
        {"checkpoint", info.checkpoint()},
        {"checkpoints", info.checkpoints},
        {"recipe", info.recipe},
        {"downloaded", info.downloaded},
        {"update_available", info.update_available},
        {"suggested", info.suggested},
        {"source", info.source.empty()
            ? remote_registry_source_name(
                parse_remote_registry_source(info.registry_source))
            : info.source},
        {"registry_source", remote_registry_source_name(
            parse_remote_registry_source(info.registry_source))},
        {"labels", info.labels},
        {"components", public_components},
        {"recipe_options", info.recipe_options.to_json()},
    };

    // Surface the cloud provider on cloud entries so the Model Manager can
    // bucket each provider into its own sub-heading. Omitted on local models
    // so the field doesn't pollute every entry — and skipped for the Ollama
    // serialization path (handle_ollama_show / tags) which builds its own
    // payload.
    if (!info.cloud_provider.empty()) {
        model_json["cloud_provider"] = info.cloud_provider;
    }

    // Add size if available
    if (info.size > 0.0) {
        model_json["size"] = info.size;
    }

    if (info.max_context_window > 0) {
        model_json["max_context_window"] = info.max_context_window;
    }

    // OpenAI-compatible clients use context_length to set token limits.
    const int64_t context_length = resolve_context_length(model_id, info);
    if (context_length > 0) {
        model_json["context_length"] = context_length;
    }

    if (info.max_output_tokens > 0) {
        model_json["max_output_tokens"] = info.max_output_tokens;
        model_json["max_completion_tokens"] = info.max_output_tokens;
    }

    // Per-million-token pricing in USD, when the provider reported it (cloud
    // models from OpenRouter/Together). Display only.
    if (info.cost_input_per_million >= 0) {
        model_json["cost_input_per_million"] = info.cost_input_per_million;
    }
    if (info.cost_output_per_million >= 0) {
        model_json["cost_output_per_million"] = info.cost_output_per_million;
    }

    // Per-collection system prompt override (collection.omni only). Omitted on
    // models that don't carry one so the field doesn't pollute every entry.
    if (!info.system_prompt.empty()) {
        model_json["system_prompt"] = info.system_prompt;
    }

    auto audio_defaults = info.extras.find("audio_defaults");
    if (audio_defaults != info.extras.end() &&
        audio_defaults->second.is_object()) {
        model_json["audio_defaults"] = audio_defaults->second;
    }

    // Add image_defaults if present (for sd-cpp models)
    if (info.image_defaults.has_defaults) {
        json img_def = {
            {"steps", info.image_defaults.steps},
            {"cfg_scale", info.image_defaults.cfg_scale},
            {"width", info.image_defaults.width},
            {"height", info.image_defaults.height}
        };
        if (!info.image_defaults.sampling_method.empty())
            img_def["sampling_method"] = info.image_defaults.sampling_method;
        if (info.image_defaults.flow_shift > 0.0f)
            img_def["flow_shift"] = info.image_defaults.flow_shift;
        if (info.image_defaults.soft_pixel_cap > 0)
            img_def["soft_pixel_cap"] = info.image_defaults.soft_pixel_cap;
        model_json["image_defaults"] = img_def;
    }

    if (is_router_collection_recipe(info.recipe)) {
        // The parser requires a root "version"; surface it alongside "routing"
        // so an exported router collection can be re-imported through /pull.
        auto version_it = info.extras.find("version");
        if (version_it != info.extras.end()) {
            model_json["version"] = version_it->second;
        }
        auto routing_it = info.extras.find("routing");
        if (routing_it != info.extras.end() && routing_it->second.is_object()) {
            model_json["routing"] = routing_it->second;
        }
    }

    // Collections additionally embed each component's full model object,
    // in component order, under "models". Embedding is bounded by
    // kMaxCollectionEmbedDepth so nested (or cyclic) collection registrations
    // cannot recurse unboundedly.
    if (is_model_collection_recipe(info.recipe) && depth < kMaxCollectionEmbedDepth) {
        nlohmann::json component_models = nlohmann::json::array();
        for (const auto& component : info.components) {
            if (!model_manager_->model_exists(component)) {
                continue;
            }
            auto comp_info = model_manager_->get_model_info(component);
            component_models.push_back(model_info_to_json(
                model_manager_->get_public_model_name(component), comp_info, depth + 1));
        }
        model_json["models"] = component_models;
    }

    return model_json;
}

void Server::handle_model_by_id(const httplib::Request& req, httplib::Response& res) {
    std::string model_id = utils::normalize_model_name(req.matches[1]);

    if (model_manager_->model_exists(model_id)) {
        auto info = model_manager_->get_model_info(model_id);
        std::string canonical_cache_key = model_manager_->resolve_model_name(model_id);
        std::string wire_id = model_manager_->get_public_model_name(canonical_cache_key);
        if (alias_manager_ && alias_manager_->has_alias(model_id)) {
            wire_id = model_id;
        }
        res.set_content(model_info_to_json(wire_id, info).dump(), "application/json");
    } else if (alias_manager_ && alias_manager_->has_alias(model_id)) {
        auto resolved_target = alias_manager_->resolve_alias(model_id);
        if (resolved_target) {
            std::string canonical_target = model_manager_->resolve_model_name(*resolved_target);
            if (model_manager_->model_exists(canonical_target)) {
                auto info = model_manager_->get_model_info(canonical_target);
                res.set_content(model_info_to_json(model_id, info).dump(), "application/json");
                return;
            } else if (model_manager_->model_exists(*resolved_target)) {
                auto info = model_manager_->get_model_info(*resolved_target);
                res.set_content(model_info_to_json(model_id, info).dump(), "application/json");
                return;
            }
        }
        res.status = 404;
        auto error_response = create_model_error(model_id, "Model not found");
        res.set_content(error_response.dump(), "application/json");
    } else {
        res.status = 404;
        auto error_response = create_model_error(model_id, "Model not found");
        res.set_content(error_response.dump(), "application/json");
    }
}

void Server::handle_model_files(const httplib::Request& req, httplib::Response& res) {
    std::string model_id = req.matches[1];
    const bool include_paths = req.has_param("include_paths") &&
        req.get_param_value("include_paths") == "true";

    try {
        if (!model_manager_->model_exists(model_id)) {
            res.status = 404;
            auto error_response = create_model_error(model_id, "Model not found");
            res.set_content(error_response.dump(), "application/json");
            return;
        }

        std::string canonical_cache_key = model_manager_->resolve_model_name(model_id);
        std::string wire_id = model_manager_->get_public_model_name(canonical_cache_key);
        auto files = model_manager_->list_model_files(model_id);

        nlohmann::json response;
        response["model_id"] = wire_id;
        response["files"] = nlohmann::json::array();

        for (const auto& file : files) {
            nlohmann::json file_json = {
                {"name", file.name},
                {"role", file.role},
                {"size_bytes", file.size_bytes},
                {"exists", file.exists}
            };

            if (include_paths) {
                file_json["path"] = file.path;
            }

            response["files"].push_back(std::move(file_json));
        }

        res.set_content(response.dump(), "application/json");
    } catch (const std::exception&) {
        res.status = 404;
        auto error_response = create_model_error(model_id, "Model not found");
        res.set_content(error_response.dump(), "application/json");
    }
}

// `pinned` is live-process state: Router::load_model keeps a running server's
// own pin rather than the resolved one, so a value saved here would not reach a
// model that is already up. /v1/load and /internal/pin own it, and this endpoint
// neither reports it in effective/defaults nor accepts it.
static bool is_live_process_option(const std::string& key) {
    return key == "pinned";
}

// Fill in every option the recipe accepts, resolving unset keys through the
// default chain, so a client can render a complete form from one response.
static nlohmann::json resolve_all_recipe_options(const RecipeOptions& options) {
    nlohmann::json resolved = options.to_resolved_json();
    resolved.erase("pinned");
    return resolved;
}

static bool option_type_matches(const nlohmann::json& expected, const nlohmann::json& value) {
    if (expected.is_number()) return value.is_number();
    if (expected.is_string()) return value.is_string();
    // Booleans, and the tri-state options whose default is null to mean "follow
    // the global setting", both only ever hold a boolean once set.
    return value.is_boolean();
}

// Reject values that pass the type check but would break the load they are
// saved for. Returns an error message, or empty when the value is usable.
static std::string validate_option_value(const std::string& key,
                                         const nlohmann::json& expected,
                                         const nlohmann::json& value) {
    if (key == "ctx_size") {
        // A literal above INT64_MAX parses as an unsigned and wraps on the way
        // to int64_t, so 2^64-1 would arrive as -1 and read as "size it
        // automatically". Rule it out before the conversion.
        const bool fits_int64 = value.is_number_integer() &&
            (!value.is_number_unsigned() ||
             value.get<uint64_t>() <= static_cast<uint64_t>(INT64_MAX));
        // 0 is not a shorthand for anything here: only -1 auto-resolves, so a 0
        // reaches the backend verbatim and FastFlowLM and vLLM both reject it.
        if (!fits_int64 || value.get<int64_t>() < -1 || value == 0) {
            return "'ctx_size' must be a positive whole number, "
                   "or -1 to size it automatically";
        }
        return "";
    }

    // A fractional value for a whole-number option is silently ignored by the
    // consumers that read it, so refuse it.
    if (expected.is_number_integer() && !value.is_number_integer()) {
        return "'" + key + "' must be a whole number";
    }
    return "";
}

void Server::respond_with_model_options(
    const httplib::Request& req, httplib::Response& res,
    const std::function<bool(const std::string&, ModelInfo&, httplib::Response&)>& mutation) {
    const std::string model_id = utils::normalize_model_name(req.matches[1]);
    std::string model_key = model_id;

    if (!model_manager_->model_exists(model_key)) {
        std::optional<std::string> target;
        if (alias_manager_ && alias_manager_->has_alias(model_id)) {
            target = alias_manager_->resolve_alias(model_id);
        }
        std::string canonical_target = target ? model_manager_->resolve_model_name(*target) : "";
        if (target && model_manager_->model_exists(canonical_target)) {
            model_key = canonical_target;
        } else if (target && model_manager_->model_exists(*target)) {
            model_key = *target;
        } else {
            res.status = 404;
            res.set_content(create_model_error(model_id, "Model not found").dump(), "application/json");
            return;
        }
    }

    // Work in canonical names from here on: a user model's requested id is its
    // bare public name, which not every downstream lookup resolves for itself.
    model_key = model_manager_->resolve_model_name(model_key);

    try {
        ModelInfo info = model_manager_->get_model_info(model_key);
        if (mutation && !mutation(model_key, info, res)) return;

        const RecipeOptions no_request_options(info.recipe, nlohmann::json::object());
        RecipeOptions effective = router_->resolve_effective_options(info, no_request_options);

        ModelInfo without_saved = info;
        without_saved.recipe_options = model_manager_->get_model_default_options(info);
        RecipeOptions defaults = router_->resolve_effective_options(without_saved, no_request_options);

        // `effective` and `defaults` double as replayable /v1/load bodies.
        nlohmann::json effective_json = resolve_all_recipe_options(effective);
        nlohmann::json defaults_json = resolve_all_recipe_options(defaults);
        effective_json["model_name"] = model_id;
        defaults_json["model_name"] = model_id;

        const int64_t auto_ctx = resolve_auto_ctx_size(effective, info);
        const nlohmann::json effective_ctx = effective.get_option("ctx_size");
        const int64_t resolved_ctx = auto_ctx != -2 ? auto_ctx
            : (effective_ctx.is_number() ? effective_ctx.get<int64_t>() : -1);

        nlohmann::json response = {
            {"model_name", model_id},
            {"recipe", info.recipe},
            {"saved", model_manager_->get_saved_model_options(model_key)},
            {"effective", effective_json},
            {"defaults", defaults_json},
            {"resolved_ctx_size", resolved_ctx}
        };
        res.set_content(response.dump(), "application/json");
    } catch (const std::exception& e) {
        LOG(ERROR, "Server") << "Failed to handle options for '" << model_id
                             << "': " << e.what() << std::endl;
        if (!model_manager_->model_exists(model_key)) {
            // The model went away between the existence check and the read.
            res.status = 404;
            res.set_content(create_model_error(model_key, e.what()).dump(), "application/json");
            return;
        }
        // Anything else is a server-side failure, most often the options file
        // being unwritable. Reporting it through create_model_error would call
        // it a load failure, which this endpoint never performs.
        res.status = 500;
        nlohmann::json error = {{"error", {
            {"message", "Failed to read or update options for '" + model_id + "': " + e.what()},
            {"type", "server_error"},
            {"code", "internal_error"}
        }}};
        res.set_content(error.dump(), "application/json");
    }
}

void Server::handle_model_options_get(const httplib::Request& req, httplib::Response& res) {
    respond_with_model_options(req, res, nullptr);
}

void Server::handle_model_options_post(const httplib::Request& req, httplib::Response& res) {
    respond_with_model_options(req, res,
        [this, &req](const std::string& model_key, ModelInfo& info, httplib::Response& r) {
            nlohmann::json body;
            if (!parse_required_json_body(req, r, body)) return false;
            if (!body.is_object()) {
                r.status = 400;
                r.set_content(nlohmann::json{{"error", "Request body must be a JSON object of recipe options"}}
                                  .dump(), "application/json");
                return false;
            }

            bool dry_run = false;
            if (body.contains("dry_run")) {
                if (!body["dry_run"].is_boolean()) {
                    r.status = 400;
                    r.set_content(nlohmann::json{{"error", "'dry_run' must be a boolean"}}
                                      .dump(), "application/json");
                    return false;
                }
                dry_run = body["dry_run"].get<bool>();
                body.erase("dry_run");
            }

            const auto keys = RecipeOptions::keys_for_recipe(info.recipe);
            const std::set<std::string> allowed(keys.begin(), keys.end());
            const RecipeOptions unset(info.recipe, nlohmann::json::object());

            // Validate everything before writing anything, and express each
            // change as set-or-erase so the merge can happen atomically.
            nlohmann::json changes = nlohmann::json::object();
            for (const auto& [key, value] : body.items()) {
                // The URL names the model; tolerate the body's copy so the
                // `effective` object (a /v1/load body) can be posted back here.
                if (key == "model_name") continue;
                if (!allowed.count(key)) {
                    r.status = 400;
                    r.set_content(nlohmann::json{{"error", "Unknown option '" + key +
                                                           "' for recipe '" + info.recipe + "'"}}
                                      .dump(), "application/json");
                    return false;
                }
                if (is_live_process_option(key)) {
                    r.status = 400;
                    r.set_content(nlohmann::json{{"error", "'" + key + "' applies to a running "
                        "model; use /v1/load or /internal/pin"}}.dump(), "application/json");
                    return false;
                }
                // null removes the key. The other values the option system
                // reads as "not set" ("", -1, "auto") still clear every option
                // except ctx_size, where -1 is a value and a malformed size is
                // an error rather than an absence.
                const bool clears = value.is_null() ||
                    (key != "ctx_size" && RecipeOptions::is_default_sentinel(key, value));
                if (clears) {
                    changes[key] = nullptr;
                    continue;
                }
                const nlohmann::json expected = unset.get_option(key);
                if (!option_type_matches(expected, value)) {
                    r.status = 400;
                    r.set_content(nlohmann::json{{"error", "Invalid type for option '" + key + "'"}}
                                      .dump(), "application/json");
                    return false;
                }
                const std::string invalid = validate_option_value(key, expected, value);
                if (!invalid.empty()) {
                    r.status = 400;
                    r.set_content(nlohmann::json{{"error", invalid}}.dump(), "application/json");
                    return false;
                }
                changes[key] = value;
            }

            if (dry_run) {
                info.recipe_options = model_manager_->preview_saved_model_options(info, changes);
                return true;
            }
            if (!changes.empty()) {
                model_manager_->update_saved_model_options(model_key, changes);
            }
            info = model_manager_->get_model_info(model_key);
            return true;
        });
}

void Server::handle_model_options_delete(const httplib::Request& req, httplib::Response& res) {
    respond_with_model_options(req, res,
        [this](const std::string& model_key, ModelInfo& info, httplib::Response&) {
            if (!model_manager_->get_saved_model_options(model_key).empty()) {
                model_manager_->set_saved_model_options(model_key, nlohmann::json::object());
            }
            info = model_manager_->get_model_info(model_key);
            return true;
        });
}

void Server::handle_collection_chat_completions(const nlohmann::json& request_json,
                                                const ModelInfo& collection_info,
                                                httplib::Response& res) {
    // Load the whole collection up front (shared, collection-aware loader) so the
    // model is fully ready — not just its chat component.
    try {
        ensure_collection_loaded(collection_info);
    } catch (const std::exception& e) {
        LOG(ERROR, "Server") << "Failed to load collection '" << collection_info.model_name
                             << "': " << e.what() << std::endl;
        auto error_response = create_model_error(collection_info.model_name, e.what());
        std::string error_code = error_response["error"]["code"].get<std::string>();
        res.status = get_http_status_from_error(error_code);
        res.set_content(error_response.dump(), "application/json");
        return;
    }

    const bool is_streaming = request_json.contains("stream") &&
                              request_json["stream"].is_boolean() &&
                              request_json["stream"].get<bool>();

    if (is_streaming) {
        LOG(INFO, "Server") << "POST /api/v1/chat/completions - Collection (streaming): "
                            << collection_info.model_name << std::endl;
        res.set_header("Cache-Control", "no-cache");
        res.set_header("Connection", "keep-alive");
        res.set_header("X-Accel-Buffering", "no");
        res.set_chunked_content_provider(
            "text/event-stream",
            [this, request_json, collection_info](size_t offset, httplib::DataSink& sink) {
                if (offset > 0) return false;  // single pass
                CollectionOrchestrator orchestrator(
                    *router_, *model_manager_,
                    [this](const std::string& m) { auto_load_model_if_needed(m); });
                try {
                    orchestrator.chat_completion_stream(request_json, collection_info, sink);
                } catch (const std::exception& e) {
                    LOG(ERROR, "Server") << "Collection streaming failed: " << e.what() << std::endl;
                }
                return false;
            });
        return;
    }

    LOG(INFO, "Server") << "POST /api/v1/chat/completions - Collection: "
                        << collection_info.model_name << std::endl;
    CollectionOrchestrator orchestrator(
        *router_, *model_manager_,
        [this](const std::string& m) { auto_load_model_if_needed(m); });
    json response = orchestrator.chat_completion(request_json, collection_info);
    if (response.contains("error")) {
        set_error_response(response, res);
        return;
    }
    res.set_content(response.dump(), "application/json");
}

std::optional<RouterDispatchResult> Server::route_collection_request(
    const nlohmann::json& request_json,
    const ModelInfo& collection_info) {
    // The policy is parsed once when the models cache is built (ModelManager),
    // so dispatch just reads it here. A missing policy means the collection
    // failed to parse at cache-build time; return nullopt so the caller leaves
    // the request's model field untouched (fail open).
    if (!collection_info.route_policy) {
        LOG(WARNING, "Server") << "Router collection '" << collection_info.model_name
                               << "' has no parsed routing policy" << std::endl;
        return std::nullopt;
    }

    // The engine owns its policy (and is rebuilt per request because its
    // classifier services are bound to the live Router), so copy the shared,
    // immutable policy into it.
    RoutePolicy policy = *collection_info.route_policy;
    ClassifierServices services = make_router_classifier_services(
        *router_, [this](const std::string& m) {
            auto_load_model_if_needed(m, json::object(),
                                      LoadPurpose::RoutingDependency);
        });
    CostServices cost_services = make_router_cost_services(*router_);
    RoutingPolicyEngine engine(std::move(policy), std::move(services),
                               std::move(cost_services));

    RouteContext ctx = build_route_context(request_json, collection_info.model_name);
    const bool want_trace = request_json.value("route_trace", false);
    Decision decision = engine.route(ctx, want_trace);
    RouterDispatchResult result;
    result.requested_model = collection_info.model_name;
    result.selected_model = decision.route_to;
    result.decision = std::move(decision);
    router_->note_route_decision(utils::conversation_fingerprint(request_json),
                                 result.selected_model);
    return result;
}

void Server::handle_routing_validate(const httplib::Request& req, httplib::Response& res) {
    nlohmann::json request_json;
    if (!parse_required_json_body(req, res, request_json)) return;

    if (!request_json.contains("policy") || !request_json["policy"].is_object()) {
        res.status = 400;
        nlohmann::json error = {{"error", "'policy' must be a JSON object"}};
        res.set_content(error.dump(), "application/json");
        return;
    }

    // A real request body (chat's `messages`, an explicit `tools` array, the
    // Responses API's `input`, or the completions form's multi-segment array
    // `prompt`) opts into deriving RouteContext via build_route_context, the
    // same function production uses — this is what lets the preview pick up
    // any signal that function knows how to compute (multi-turn text, image
    // content parts, tool arrays) without hand-plumbing each one here, and
    // it's why the flattened has_images/has_tools/prompt shape below is kept
    // working unchanged rather than being routed through the same function.
    const bool prompt_present = request_json.contains("prompt");
    const bool prompt_is_array = prompt_present && request_json["prompt"].is_array();
    const bool real_body = request_json.contains("messages") ||
                            request_json.contains("input") ||
                            request_json.contains("tools") || prompt_is_array;

    // Presence alone decides `real_body` above; without these, a wrong-typed
    // messages/input/tools would still flip into real-body mode and then
    // silently evaluate as empty (build_route_context's own type guards treat
    // a malformed field as absent, not an error) — a confidently-wrong 200
    // instead of a 400 naming the mistake.
    if (prompt_present && !request_json["prompt"].is_string() && !prompt_is_array) {
        res.status = 400;
        nlohmann::json error = {{"error", "'prompt' must be a string or an array"}};
        res.set_content(error.dump(), "application/json");
        return;
    }

    const int primary_body_fields = static_cast<int>(request_json.contains("messages")) +
                                    static_cast<int>(request_json.contains("input")) +
                                    static_cast<int>(prompt_present);
    if (primary_body_fields > 1) {
        res.status = 400;
        nlohmann::json error = {{"error",
            "only one of 'messages', 'input', or 'prompt' may be provided"}};
        res.set_content(error.dump(), "application/json");
        return;
    }

    if (request_json.contains("messages") && !request_json["messages"].is_array()) {
        res.status = 400;
        nlohmann::json error = {{"error", "'messages' must be an array"}};
        res.set_content(error.dump(), "application/json");
        return;
    }

    if (request_json.contains("input") && !request_json["input"].is_string() &&
        !request_json["input"].is_array()) {
        res.status = 400;
        nlohmann::json error = {{"error", "'input' must be a string or an array"}};
        res.set_content(error.dump(), "application/json");
        return;
    }

    if (request_json.contains("tools") && !request_json["tools"].is_array()) {
        res.status = 400;
        nlohmann::json error = {{"error", "'tools' must be an array"}};
        res.set_content(error.dump(), "application/json");
        return;
    }

    if (prompt_is_array) {
        for (const auto& part : request_json["prompt"]) {
            if (!part.is_string()) {
                res.status = 400;
                nlohmann::json error = {{"error", "'prompt' array elements must be strings"}};
                res.set_content(error.dump(), "application/json");
                return;
            }
        }

        auto validate_content = [](const nlohmann::json& content,
                                   const char* field) -> std::optional<std::string> {
            if (content.is_string()) return std::nullopt;
            if (!content.is_array()) {
                return std::string("'") + field + "' content must be a string or an array";
            }
            for (const auto& part : content) {
                if (!part.is_object()) {
                    return std::string("'") + field + "' content parts must be objects";
                }
                const std::string type = part.value("type", std::string());
                if ((type == "text" || type == "input_text") &&
                    (!part.contains("text") || !part["text"].is_string())) {
                    return std::string("'") + field + "' text parts must contain string 'text'";
                }
            }
            return std::nullopt;
        };

        if (request_json.contains("messages")) {
            for (const auto& message : request_json["messages"]) {
                if (!message.is_object() || !message.contains("content")) {
                    res.status = 400;
                    nlohmann::json error = {{"error",
                        "'messages' items must be objects with a 'content' field"}};
                    res.set_content(error.dump(), "application/json");
                    return;
                }
                if (auto error_message = validate_content(message["content"], "messages")) {
                    res.status = 400;
                    nlohmann::json error = {{"error", *error_message}};
                    res.set_content(error.dump(), "application/json");
                    return;
                }
            }
        }

        if (request_json.contains("input") && request_json["input"].is_array()) {
            for (const auto& item : request_json["input"]) {
                if (item.is_string()) continue;
                if (!item.is_object()) {
                    res.status = 400;
                    nlohmann::json error = {{"error",
                        "'input' array items must be strings or objects"}};
                    res.set_content(error.dump(), "application/json");
                    return;
                }
                const nlohmann::json content =
                    item.contains("content") ? item["content"] : nlohmann::json::array({item});
                if (auto error_message = validate_content(content, "input")) {
                    res.status = 400;
                    nlohmann::json error = {{"error", *error_message}};
                    res.set_content(error.dump(), "application/json");
                    return;
                }
            }
        }
    }

    if (real_body &&
        (request_json.contains("has_images") || request_json.contains("has_tools"))) {
        res.status = 400;
        nlohmann::json error = {{"error",
            "cannot combine 'has_images'/'has_tools' with a real request body "
            "('messages', 'input', 'tools', or an array 'prompt'); use one shape "
            "or the other"}};
        res.set_content(error.dump(), "application/json");
        return;
    }

    if (request_json.contains("has_images") && !request_json["has_images"].is_boolean()) {
        res.status = 400;
        nlohmann::json error = {{"error", "'has_images' must be a boolean"}};
        res.set_content(error.dump(), "application/json");
        return;
    }
    const bool has_images = request_json.value("has_images", false);

    if (request_json.contains("has_tools") && !request_json["has_tools"].is_boolean()) {
        res.status = 400;
        nlohmann::json error = {{"error", "'has_tools' must be a boolean"}};
        res.set_content(error.dump(), "application/json");
        return;
    }
    const bool has_tools = request_json.value("has_tools", false);

    if (request_json.contains("metadata")) {
        const nlohmann::json& metadata_json = request_json["metadata"];
        if (!metadata_json.is_object()) {
            res.status = 400;
            nlohmann::json error = {{"error", "'metadata' must be a JSON object of string values"}};
            res.set_content(error.dump(), "application/json");
            return;
        }
        for (auto it = metadata_json.begin(); it != metadata_json.end(); ++it) {
            if (!it.value().is_string()) {
                res.status = 400;
                nlohmann::json error = {{"error",
                    "'metadata' value for key '" + it.key() + "' must be a string"}};
                res.set_content(error.dump(), "application/json");
                return;
            }
        }
    }

    try {
        // Ad-hoc validation: the policy isn't attached to a registered model,
        // so accept any candidate/component name as-is rather than requiring
        // it to resolve against the live model registry. This identity
        // resolver is the only relaxation from the normal registration path —
        // require_declared_components stays at its default (true) so the
        // document must still be internally consistent (every candidate,
        // default_model, rule route_to, and classifier model must be listed
        // in collection.components), matching what registration would accept.
        RoutingPolicyParseOptions options;
        options.resolve_component = [](const std::string& name) -> std::optional<std::string> {
            return name;
        };
        nlohmann::json normalized_routing;
        RoutePolicy policy = parse_route_policy_collection(
            request_json["policy"], options, &normalized_routing);

        ClassifierServices services = make_router_classifier_services(
            *router_, [this](const std::string& m) {
                auto_load_model_if_needed(m, json::object(),
                                          LoadPurpose::RoutingDependency);
            });
        RoutingPolicyEngine engine(std::move(policy), std::move(services));

        // Always derive ctx from build_route_context, the same function
        // production uses, so every Params field (present today or added
        // later, e.g. total_chars) comes from one place instead of being
        // re-plumbed by hand here. A flattened legacy request has no
        // messages/tools content for build_route_context to detect
        // has_images/has_tools from, though — those two booleans are the
        // caller's literal signal values, not something to derive, so
        // they're the one deliberate override. (build_route_context's
        // history-less `prompt` branch already sets total_chars to the same
        // prompt.size() a hand-built ctx would need to set explicitly here.)
        RouteContext ctx = build_route_context(request_json, std::string());
        if (!real_body) {
            ctx.params.has_images = has_images;
            ctx.params.has_tools = has_tools;
        }

        Decision decision = engine.route(ctx, /*want_trace=*/true);

        // Echo back the policy actually evaluated (`routing.router` desugared
        // into explicit `classifiers`/`rules`, e.g. `__route_0`) so a caller
        // can match `decision.matched_rule` against a rule that exists —
        // the as-authored policy may only have `routing.router` and no
        // `routing.rules` at all.
        nlohmann::json normalized_policy = request_json["policy"];
        normalized_policy["routing"] = std::move(normalized_routing);

        nlohmann::json response = {
            {"decision", route_decision_to_json(decision)},
            {"normalized_policy", std::move(normalized_policy)},
        };
        res.set_content(response.dump(), "application/json");
    } catch (const std::exception& e) {
        res.status = 400;
        nlohmann::json error = {{"error", std::string("Invalid routing policy: ") + e.what()}};
        res.set_content(error.dump(), "application/json");
    }
}

std::optional<RouterDispatchResult> Server::apply_router_collection_dispatch(
    nlohmann::json& request_json) {
    if (!request_json.contains("model") || !request_json["model"].is_string()) {
        return std::nullopt;
    }
    const std::string requested_model = request_json["model"].get<std::string>();
    try {
        if (!model_manager_->model_exists(requested_model)) {
            return std::nullopt;
        }
        ModelInfo info = model_manager_->get_model_info(requested_model);
        if (!is_router_collection_recipe(info.recipe)) {
            return std::nullopt;
        }
        auto dispatch = route_collection_request(request_json, info);
        if (!dispatch) {
            return std::nullopt;
        }
        dispatch->requested_model = requested_model;
        LOG(INFO, "Server") << "Router collection '" << requested_model << "' -> '"
                            << dispatch->selected_model << "'" << std::endl;
        request_json["model"] = dispatch->selected_model;
        request_json.erase("route_trace");
        return dispatch;
    } catch (const RouterResidencyConflictException&) {
        // This is a deterministic hardware-policy conflict, not a routing miss.
        // Let the endpoint serialize it as HTTP 409 instead of silently falling
        // back to trying to load the collection recipe itself.
        throw;
    } catch (const std::exception& e) {
        LOG(WARNING, "Server") << "Router collection dispatch failed for '"
                               << requested_model << "': " << e.what() << std::endl;
    }
    return std::nullopt;
}

std::set<std::string> Server::active_policy_helper_models() {
    std::set<std::string> needed;
    for (const auto& [name, info] : model_manager_->get_supported_models()) {
        (void)name;
        if (!info.route_policy) {
            continue;
        }
        for (const auto& helper : info.route_policy->helper_models) {
            needed.insert(helper);
        }
    }
    return needed;
}

void Server::record_response_telemetry(const nlohmann::json& response,
                                       const nlohmann::json& request_json) {
    if (!response.is_object() ||
        (!response.contains("timings") && !response.contains("usage"))) {
        return;
    }

    StreamingProxy::TelemetryData telemetry = StreamingProxy::extract_telemetry(response);
    std::string model_name = request_json.value("model", "");
    LOG(INFO, "Telemetry") << "Inference completed: model=" << model_name
                           << ", tokens=" << (telemetry.input_tokens + telemetry.output_tokens)
                           << " (in=" << telemetry.input_tokens
                           << ", out=" << telemetry.output_tokens << ")"
                           << ", ttft=" << std::fixed << std::setprecision(2)
                           << telemetry.time_to_first_token << "s"
                           << ", tps=" << telemetry.tokens_per_second << std::endl;

    router_->update_request_telemetry(model_name, telemetry);
}

void Server::handle_chat_completions(const httplib::Request& req, httplib::Response& res) {
    nlohmann::json request_json;
    if (!parse_required_json_body(req, res, request_json)) return;

    utils::RequestCancelToken cancel_token;
    if (req.is_connection_closed) {
        cancel_token.should_cancel = [is_closed = req.is_connection_closed]() { return is_closed && is_closed(); };
    }
    WrappedServer::RequestCancelScope cancel_scope(cancel_token);

    try {

        // Normalize client-provided model names (e.g., strip ":latest" suffix)
        // Must be done before any model_manager/router lookups and before forwarding
        normalize_client_model_name(request_json);
        normalize_and_resolve_request_model(request_json);

        // Debug: Check if tools are present
        if (request_json.contains("tools")) {
            LOG(DEBUG, "Server") << "Tools present in request: " << request_json["tools"].size() << " tool(s)" << std::endl;
            LOG(DEBUG, "Server") << "Tools JSON: " << request_json["tools"].dump() << std::endl;
        } else {
            LOG(DEBUG, "Server") << "No tools in request" << std::endl;
        }

        std::optional<RouterDispatchResult> route_dispatch;

        // Omni "collection" models run a server-side tool-calling loop instead of a
        // plain completion. Branch before auto-load/LLM-type checks: the collection
        // recipe has no backend of its own; the orchestrator loads each component.
        if (request_json.contains("model") && request_json["model"].is_string()) {
            const std::string requested_model = request_json["model"].get<std::string>();
            try {
                if (model_manager_->model_exists(requested_model)) {
                    ModelInfo info = model_manager_->get_model_info(requested_model);
                    if (is_omni_collection_recipe(info.recipe)) {
                        handle_collection_chat_completions(request_json, info, res);
                        return;
                    }
                    if (is_router_collection_recipe(info.recipe)) {
                        // The recipe is the trigger (no "auto", no /v1/route): run the
                        // routing engine and rewrite the model to the selected
                        // candidate, then fall through to normal completion handling.
                        route_dispatch = route_collection_request(request_json, info);
                        if (route_dispatch) {
                            route_dispatch->requested_model = requested_model;
                            LOG(INFO, "Server") << "Router collection '" << requested_model
                                                << "' -> '" << route_dispatch->selected_model << "'" << std::endl;
                            request_json["model"] = route_dispatch->selected_model;
                            request_json.erase("route_trace");
                        }
                    }
                }
            } catch (const RouterResidencyConflictException& e) {
                LOG(WARNING, "Server") << "Router residency conflict for '"
                                       << requested_model << "': " << e.what() << std::endl;
                set_router_residency_conflict_response(e, res);
                return;
            } catch (const std::exception& e) {
                LOG(DEBUG, "Server") << "Collection check failed for '" << requested_model
                                     << "': " << e.what() << std::endl;
            }
        }

        std::string requested_model;
        if (request_json.contains("model") && request_json["model"].is_string()) {
            requested_model = request_json["model"].get<std::string>();
        }
        auto span = telemetry::TelemetryTracker::start_span("LLM", "chat.completions", requested_model, request_json);

        // Handle model loading/switching
        if (request_json.contains("model")) {
            try {
                auto_load_model_if_needed(requested_model, extract_auto_load_options(request_json));
                if (span) {
                    span->cancel();
                }
            } catch (const std::exception& e) {
                LOG(ERROR, "Server") << "Failed to load model: " << e.what() << std::endl;
                auto error_response = create_model_error(requested_model, e.what());
                // Set appropriate status code based on error type
                std::string error_code = error_response["error"]["code"].get<std::string>();
                res.status = get_http_status_from_error(error_code);
                res.set_content(error_response.dump(), "application/json");

                if (span) {
                    span->end_with_error(e.what());
                }
                return;
            }
        } else if (!router_->is_model_loaded()) {
            LOG(ERROR, "Server") << "No model loaded and no model specified in request" << std::endl;
            res.status = 400;
            res.set_content("{\"error\": \"No model loaded and no model specified in request\"}", "application/json");
            if (span) {
                span->cancel();
            }
            return;
        }

        // Check if the loaded model supports chat completion (only LLM models do)
        std::string model_to_check = request_json.contains("model") ? request_json["model"].get<std::string>() : "";
        if (router_->get_model_type(model_to_check) != ModelType::LLM) {
            LOG(ERROR, "Server") << "Model does not support chat completion" << std::endl;
            res.status = 400;
            res.set_content(R"({"error": {"message": "This model does not support chat completion. Only LLM models support this endpoint.", "type": "invalid_request_error"}})", "application/json");
            if (span) {
                span->cancel();
            }
            return;
        }

        if (span) {
            span->cancel();
        }

        // Check if streaming is requested
        bool is_streaming = request_json.contains("stream") && request_json["stream"].get<bool>();

        // OpenCode and other OpenAI-compatible clients may send thinking=false
        // instead of Lemonade's enable_thinking=false.
        normalize_thinking_controls(request_json);

        if (is_streaming) {
            try {
                std::string request_body = request_json.dump();
                // Log the HTTP request
                LOG(INFO, "Server") << "POST /api/v1/chat/completions - Streaming" << std::endl;

                set_route_decision_sse_content_provider(
                    res,
                    route_dispatch,
                    request_body,
                    [this](const std::string& body, httplib::DataSink& sink) {
                        router_->chat_completion_stream(body, sink);
                    });
            } catch (const std::exception& e) {
                LOG(ERROR, "Server") << "Streaming failed: " << e.what() << std::endl;
                res.status = 500;
                res.set_content("{\"error\":\"Internal server error during streaming\"}", "application/json");
            }
        } else {
            // Log the HTTP request
            LOG(INFO, "Server") << "POST /api/v1/chat/completions - 200 OK" << std::endl;

            auto response = router_->chat_completion(request_json);

            if (response.contains("error")) {
                LOG(ERROR, "Server") << "Backend returned error response: " << response["error"].dump() << std::endl;
                set_error_response(response, res);
                return;
            }

            attach_route_decision(response, res, route_dispatch);
            // Debug: Check if response contains tool_calls
            if (response.contains("choices") && response["choices"].is_array() && !response["choices"].empty()) {
                auto& first_choice = response["choices"][0];
                if (first_choice.contains("message")) {
                    auto& message = first_choice["message"];
                    if (message.contains("tool_calls")) {
                        LOG(DEBUG, "Server") << "Response contains tool_calls: " << message["tool_calls"].dump() << std::endl;
                    } else {
                        LOG(DEBUG, "Server") << "Response message does NOT contain tool_calls" << std::endl;
                        if (message.contains("content")) {
                            LOG(DEBUG, "Server") << "Message content: " << message["content"].get<std::string>().substr(0, 200) << std::endl;
                        }
                    }
                }
            }

            res.set_content(response.dump(), "application/json");

            record_response_telemetry(response, request_json);


        }

    } catch (const std::exception& e) {
        LOG(ERROR, "Server") << "Chat completion failed: " << e.what() << std::endl;

        res.status = 500;
        nlohmann::json error = {{"error", e.what()}};
        res.set_content(error.dump(), "application/json");
    }
}

void Server::handle_completions(const httplib::Request& req, httplib::Response& res) {
    utils::RequestCancelToken cancel_token;
    if (req.is_connection_closed) {
        cancel_token.should_cancel = [is_closed = req.is_connection_closed]() { return is_closed && is_closed(); };
    }
    WrappedServer::RequestCancelScope cancel_scope(cancel_token);

    try {
        auto request_json = nlohmann::json::parse(req.body);

        // Normalize client-provided model names (e.g., strip ":latest" suffix)
        // Must be done before any model_manager/router lookups and before forwarding
        normalize_client_model_name(request_json);
        normalize_and_resolve_request_model(request_json);

        // A collection.router model flips this endpoint into engine mode: pick a
        // candidate and rewrite the model before the usual load/forward logic.
        std::optional<RouterDispatchResult> route_dispatch =
            apply_router_collection_dispatch(request_json);

        std::string requested_model;
        if (request_json.contains("model") && request_json["model"].is_string()) {
            requested_model = request_json["model"].get<std::string>();
        }
        auto span = telemetry::TelemetryTracker::start_span("LLM", "completions", requested_model, request_json);

        // Handle model loading/switching (same logic as chat_completions)
        if (request_json.contains("model")) {
            try {
                auto_load_model_if_needed(requested_model, extract_auto_load_options(request_json));
                if (span) {
                    span->cancel();
                }
            } catch (const std::exception& e) {
                LOG(ERROR, "Server") << "Failed to load model: " << e.what() << std::endl;
                auto error_response = create_model_error(requested_model, e.what());
                // Set appropriate status code based on error type
                std::string error_code = error_response["error"]["code"].get<std::string>();
                res.status = get_http_status_from_error(error_code);
                res.set_content(error_response.dump(), "application/json");

                if (span) {
                    span->end_with_error(e.what());
                }
                return;
            }
        } else if (!router_->is_model_loaded()) {
            LOG(ERROR, "Server") << "No model loaded and no model specified in request" << std::endl;
            res.status = 400;
            res.set_content("{\"error\": \"No model loaded and no model specified in request\"}", "application/json");
            if (span) {
                span->cancel();
            }
            return;
        }

        // Check if the loaded model supports completion (only LLM models do)
        std::string model_to_check = request_json.contains("model") ? request_json["model"].get<std::string>() : "";
        if (router_->get_model_type(model_to_check) != ModelType::LLM) {
            LOG(ERROR, "Server") << "Model does not support completion" << std::endl;
            res.status = 400;
            res.set_content(R"({"error": {"message": "This model does not support completion. Only LLM models support this endpoint.", "type": "invalid_request_error"}})", "application/json");
            if (span) {
                span->cancel();
            }
            return;
        }

        if (span) {
            span->cancel();
        }

        // Check if streaming is requested
        bool is_streaming = request_json.contains("stream") && request_json["stream"].get<bool>();

        if (is_streaming) {
            try {
                std::string request_body = request_json.dump();
                // Log the HTTP request
                LOG(INFO, "Server") << "POST /api/v1/completions - Streaming" << std::endl;

                set_route_decision_sse_content_provider(
                    res,
                    route_dispatch,
                    request_body,
                    [this](const std::string& body, httplib::DataSink& sink) {
                        router_->completion_stream(body, sink);
                    });

                LOG(INFO, "Server") << "Streaming completed - 200 OK" << std::endl;
                return;

            } catch (const std::exception& e) {
                LOG(ERROR, "Server") << "Streaming failed: " << e.what() << std::endl;
                res.status = 500;
                res.set_content("{\"error\": \"" + std::string(e.what()) + "\"}", "application/json");
                return;
            }
        } else {
            // Non-streaming
            auto response = router_->completion(request_json);

            // Check if response contains an error
            if (response.contains("error")) {
                LOG(ERROR, "Server") << "Backend returned error response: " << response["error"].dump() << std::endl;
                set_error_response(response, res);
                return;
            }

            // Verify response has required fields
            if (!response.contains("choices")) {
                LOG(ERROR, "Server") << "Response missing 'choices' field. Response: " << response.dump() << std::endl;
                res.status = 500;
                nlohmann::json error = {{"error", "Backend returned invalid response format"}};
                res.set_content(error.dump(), "application/json");
                return;
            }

            attach_route_decision(response, res, route_dispatch);
            res.set_content(response.dump(), "application/json");

            record_response_telemetry(response, request_json);
        }

    } catch (const RouterResidencyConflictException& e) {
        LOG(WARNING, "Server") << "Router residency conflict in completions: "
                               << e.what() << std::endl;
        set_router_residency_conflict_response(e, res);
    } catch (const std::exception& e) {
        LOG(ERROR, "Server") << "ERROR in handle_completions: " << e.what() << std::endl;
        res.status = 500;
        nlohmann::json error = {{"error", e.what()}};
        res.set_content(error.dump(), "application/json");
    }
}

void Server::handle_embeddings(const httplib::Request& req, httplib::Response& res) {
    try {
        auto request_json = nlohmann::json::parse(req.body);
        normalize_client_model_name(request_json);
        normalize_and_resolve_request_model(request_json);

        std::string requested_model;
        if (request_json.contains("model") && request_json["model"].is_string()) {
            requested_model = request_json["model"].get<std::string>();
        }
        auto span = telemetry::TelemetryTracker::start_span("EMBEDDING", "embeddings", requested_model, request_json);

        // Handle model loading/switching using helper function
        if (request_json.contains("model")) {
            try {
                auto_load_model_if_needed(requested_model, extract_auto_load_options(request_json));
                if (span) {
                    span->cancel();
                }
            } catch (const std::exception& e) {
                LOG(ERROR, "Server") << "Failed to load model: " << e.what() << std::endl;
                auto error_response = create_model_error(requested_model, e.what());
                std::string error_code = error_response["error"]["code"].get<std::string>();
                res.status = get_http_status_from_error(error_code);
                res.set_content(error_response.dump(), "application/json");

                if (span) {
                    span->end_with_error(e.what());
                }
                return;
            }
        } else if (!router_->is_model_loaded()) {
            LOG(ERROR, "Server") << "No model loaded and no model specified in request" << std::endl;
            res.status = 400;
            res.set_content("{\"error\": \"No model loaded and no model specified in request\"}", "application/json");
            if (span) {
                span->cancel();
            }
            return;
        }

        if (span) {
            span->cancel();
        }

        // Call router's embeddings method
        auto response = router_->embeddings(request_json);
        if (response.contains("error")) {
            set_error_response(response, res);
            return;
        }

        res.set_content(response.dump(), "application/json");

    } catch (const std::exception& e) {
        LOG(ERROR, "Server") << "ERROR in handle_embeddings: " << e.what() << std::endl;
        res.status = 500;
        nlohmann::json error = {{"error", e.what()}};
        res.set_content(error.dump(), "application/json");
    }
}

void Server::handle_reranking(const httplib::Request& req, httplib::Response& res) {
    try {
        auto request_json = nlohmann::json::parse(req.body);

        std::string requested_model;
        if (request_json.contains("model") && request_json["model"].is_string()) {
            requested_model = request_json["model"].get<std::string>();
        }
        auto span = telemetry::TelemetryTracker::start_span("RERANKER", "reranking", requested_model, request_json);

        // Handle model loading/switching using helper function
        if (request_json.contains("model")) {
            try {
                auto_load_model_if_needed(requested_model, extract_auto_load_options(request_json));
                if (span) {
                    span->cancel();
                }
            } catch (const std::exception& e) {
                LOG(ERROR, "Server") << "Failed to load model: " << e.what() << std::endl;
                auto error_response = create_model_error(requested_model, e.what());
                std::string error_code = error_response["error"]["code"].get<std::string>();
                res.status = get_http_status_from_error(error_code);
                res.set_content(error_response.dump(), "application/json");

                if (span) {
                    span->end_with_error(e.what());
                }
                return;
            }
        } else if (!router_->is_model_loaded()) {
            LOG(ERROR, "Server") << "No model loaded and no model specified in request" << std::endl;
            res.status = 400;
            res.set_content("{\"error\": \"No model loaded and no model specified in request\"}", "application/json");
            if (span) {
                span->cancel();
            }
            return;
        }

        if (span) {
            span->cancel();
        }

        // Call router's reranking method
        auto response = router_->reranking(request_json);
        if (response.contains("error")) {
            set_error_response(response, res);
            return;
        }

        res.set_content(response.dump(), "application/json");

    } catch (const std::exception& e) {
        LOG(ERROR, "Server") << "ERROR in handle_reranking: " << e.what() << std::endl;
        res.status = 500;
        nlohmann::json error = {{"error", e.what()}};
        res.set_content(error.dump(), "application/json");
    }
}

void Server::handle_classify(const httplib::Request& req, httplib::Response& res) {
    try {
        nlohmann::json request_json;
        try {
            request_json = nlohmann::json::parse(req.body);
        } catch (const nlohmann::json::parse_error& e) {
            res.status = 400;
            nlohmann::json error = {{"error", {
                {"message", std::string("Invalid JSON in request body: ") + e.what()},
                {"type", "invalid_request_error"}}}};
            res.set_content(error.dump(), "application/json");
            return;
        }

        normalize_client_model_name(request_json);
        normalize_and_resolve_request_model(request_json);

        // Reject malformed requests before any model gets loaded.
        std::string validation_error;
        bool has_text = request_json.contains("text");
        bool has_input = request_json.contains("input");
        const auto& input_field = has_text ? request_json["text"]
                                 : has_input ? request_json["input"] : request_json;
        if (request_json.contains("model") && !request_json["model"].is_string()) {
            validation_error = "'model' must be a string";
        } else if (!has_text && !has_input) {
            validation_error = "Missing 'input' (or 'text') string in classify request";
        } else if (has_text && !request_json["text"].is_string()) {
            validation_error = "'text' must be a string";
        } else if (has_input && !request_json["input"].is_string()) {
            validation_error = "'input' must be a string";
        } else if (input_field.get<std::string>().find_first_not_of(" \t\r\n") ==
                   std::string::npos) {
            validation_error = "input text must not be empty";
        } else if (request_json.contains("top_k") &&
                   (!request_json["top_k"].is_number_integer() ||
                    request_json["top_k"].get<long long>() < 1 ||
                    request_json["top_k"].get<long long>() > 1000000)) {
            validation_error = "'top_k' must be a positive integer";
        }
        if (!validation_error.empty()) {
            res.status = 400;
            nlohmann::json error = {{"error", {
                {"message", validation_error},
                {"type", "invalid_request_error"}}}};
            res.set_content(error.dump(), "application/json");
            return;
        }

        std::string requested_model;
        if (request_json.contains("model") && request_json["model"].is_string()) {
            requested_model = request_json["model"].get<std::string>();
        }
        auto span = telemetry::TelemetryTracker::start_span("CLASSIFIER", "classify", requested_model, request_json);

        // Handle model loading/switching using helper function
        if (request_json.contains("model")) {
            try {
                auto_load_model_if_needed(requested_model, extract_auto_load_options(request_json));
                if (span) {
                    span->cancel();
                }
            } catch (const std::exception& e) {
                LOG(ERROR, "Server") << "Failed to load model: " << e.what() << std::endl;
                auto error_response = create_model_error(requested_model, e.what());
                std::string error_code = error_response["error"]["code"].get<std::string>();
                res.status = get_http_status_from_error(error_code);
                res.set_content(error_response.dump(), "application/json");

                if (span) {
                    span->end_with_error(e.what());
                }
                return;
            }
        } else {
            // "model" may be omitted only when exactly one classifier is loaded.
            // The router requires a model on every request, so resolve it here
            // and put it in the request rather than letting it fail downstream.
            requested_model = router_->get_sole_loaded_model_of_type(ModelType::CLASSIFICATION);
            if (requested_model.empty()) {
                res.status = 400;
                nlohmann::json error = {{"error", {
                    {"message", "No 'model' specified and no single classification model "
                                "is loaded (load one, or name it in the request)"},
                    {"type", "invalid_request_error"}}}};
                res.set_content(error.dump(), "application/json");
                if (span) {
                    span->cancel();
                }
                return;
            }
            request_json["model"] = requested_model;
        }

        if (span) {
            span->cancel();
        }

        auto response = router_->classify(request_json);
        if (response.contains("error") && response["error"].is_object()) {
            const auto& err = response["error"];
            if (err.contains("status_code") && err["status_code"].is_number_integer()) {
                res.status = err["status_code"].get<int>();
            } else if (err.contains("code") && err["code"].is_string()) {
                res.status = get_http_status_from_error(err["code"].get<std::string>());
            } else if (err.contains("type") && err["type"].is_string()) {
                // LemonException::to_json carries only {message, type}.
                const std::string type = err["type"].get<std::string>();
                if (type == "invalid_request" || type == "invalid_request_error" ||
                    type == "unsupported_operation") {
                    res.status = 400;
                } else if (type == "model_not_loaded") {
                    res.status = 404;
                } else {
                    res.status = 500;
                }
            } else {
                res.status = 500;
            }
            res.set_content(response.dump(), "application/json");
            return;
        }

        // Envelope pins the public API shape; the backend subprocess's raw
        // output is not the contract.
        if (!response.contains("labels") || !response["labels"].is_object()) {
            res.status = 500;
            nlohmann::json error = {{"error", {
                {"message", "Classification backend returned an unexpected response"},
                {"type", "classification_error"}}}};
            res.set_content(error.dump(), "application/json");
            return;
        }
        nlohmann::json enveloped = {
            {"object", "classification"},
            {"model", requested_model.empty() ? router_->get_loaded_model() : requested_model},
            {"labels", response["labels"]},
        };
        res.set_content(enveloped.dump(), "application/json");

    } catch (const std::exception& e) {
        LOG(ERROR, "Server") << "ERROR in handle_classify: " << e.what() << std::endl;
        res.status = 500;
        nlohmann::json error = {{"error", e.what()}};
        res.set_content(error.dump(), "application/json");
    }
}

void Server::handle_slots(const httplib::Request& req, httplib::Response& res) {
    try {
        // Slots endpoint doesn't require a model parameter since it queries server state
        // But we still need a model to be loaded to have an active server to query
        if (!router_->is_model_loaded()) {
            LOG(ERROR, "Server") << "No model loaded for slots query" << std::endl;
            res.status = 400;
            res.set_content("{\"error\": \"No model loaded for slots query\"}", "application/json");
            return;
        }

        // Call router's get_slots method
        auto response = router_->get_slots();
        if (response.contains("error")) {
            set_error_response(response, res);
            return;
        }

        res.set_content(response.dump(), "application/json");

    } catch (const std::exception& e) {
        LOG(ERROR, "Server") << "ERROR in handle_slots: " << e.what() << std::endl;
        res.status = 500;
        nlohmann::json error = {{"error", e.what()}};
        res.set_content(error.dump(), "application/json");
    }
}

void Server::handle_slots_by_id(const httplib::Request& req, httplib::Response& res) {
    try {
        // Extract slot ID from path parameter
        std::string slot_id_str = req.matches[1];
        int slot_id;
        try {
            slot_id = std::stoi(slot_id_str);
        } catch (const std::exception& e) {
            LOG(ERROR, "Server") << "Invalid slot ID: " << slot_id_str << std::endl;
            res.status = 400;
            res.set_content("{\"error\": \"Invalid slot ID: " + slot_id_str + "\"}", "application/json");
            return;
        }

        // Check for action query parameter
        auto action_param = req.get_param_value("action");
        if (action_param.empty()) {
            LOG(ERROR, "Server") << "Missing action parameter for slots POST endpoint" << std::endl;
            res.status = 400;
            res.set_content("{\"error\": \"POST /api/v1/slots/{id} requires action query parameter (e.g., ?action=erase)\"}", "application/json");
            return;
        }

        // Validate known actions for specific slots (can be extended as needed)
        if (action_param != "erase" && action_param != "save" && action_param != "restore") {
            LOG(ERROR, "Server") << "Unknown action parameter: " << action_param << std::endl;
            res.status = 400;
            res.set_content("{\"error\": \"Unknown action: " + action_param + ". Supported actions: erase, save, restore\"}", "application/json");
            return;
        }

        // Slots actions don't require a model parameter since they operate on server state
        // But we still need a model to be loaded to have an active server to operate on
        if (!router_->is_model_loaded()) {
            LOG(ERROR, "Server") << "No model loaded for slots " << action_param << " operation" << std::endl;
            res.status = 400;
            res.set_content("{\"error\": \"No model loaded for slots " + action_param + " operation\"}", "application/json");
            return;
        }

        // Parse request body as JSON (use empty object if body is empty)
        json request_body;
        if (!req.body.empty()) {
            try {
                request_body = json::parse(req.body);
            } catch (const std::exception& e) {
                LOG(ERROR, "Server") << "Failed to parse request body: " << e.what() << std::endl;
                res.status = 400;
                res.set_content("{\"error\": \"Invalid JSON in request body\"}", "application/json");
                return;
            }
        } else {
            request_body = json::object();
        }

        // Call router's slots_action method with slot ID, action, and request body
        auto response = router_->slots_action(slot_id, action_param, request_body);
        if (response.contains("error")) {
            set_error_response(response, res);
            return;
        }

        res.set_content(response.dump(), "application/json");

    } catch (const std::exception& e) {
        LOG(ERROR, "Server") << "ERROR in handle_slots_by_id: " << e.what() << std::endl;
        res.status = 500;
        nlohmann::json error = {{"error", e.what()}};
        res.set_content(error.dump(), "application/json");
    }
}

void Server::handle_tokenize(const httplib::Request& req, httplib::Response& res) {
    try {
        LOG(INFO, "Server") << "POST /api/v1/tokenize" << std::endl;

        // Parse request body as JSON (use empty object if body is empty)
        json request_body;
        if (!req.body.empty()) {
            try {
                request_body = json::parse(req.body);
            } catch (const std::exception& e) {
                LOG(ERROR, "Server") << "Failed to parse request body: " << e.what() << std::endl;
                res.status = 400;
                res.set_content("{\"error\": \"Invalid JSON in request body\"}", "application/json");
                return;
            }
        } else {
            request_body = json::object();
        }

        // Tokenize endpoint requires at least a valid "content" entry in the body
        if (!request_body.contains("content") || !request_body["content"].is_string()) {
            LOG(ERROR, "Server") << "Tokenization failed: 'content' parameter is missing" << std::endl;
            res.status = 400;
            res.set_content("{\"error\": \"'content' parameter is required\"}", "application/json");
            return;
        }

        // Tokenization requires a model to be loaded
        if (!router_->is_model_loaded()) {
            LOG(ERROR, "Server") << "No model loaded for tokenization" << std::endl;
            res.status = 400;
            res.set_content("{\"error\": \"No model loaded for tokenization\"}", "application/json");
            return;
        }

        // Forward request to router
        auto response = router_->tokenize(request_body);
        if (response.contains("error")) {
            set_error_response(response, res);
            return;
        }

        res.set_content(response.dump(), "application/json");

    } catch (const std::exception& e) {
        LOG(ERROR, "Server") << "ERROR in handle_tokenize: " << e.what() << std::endl;
        res.status = 500;
        nlohmann::json error = {{"error", e.what()}};
        res.set_content(error.dump(), "application/json");
    }
}

void Server::handle_audio_transcriptions(const httplib::Request& req, httplib::Response& res) {
    try {
        LOG(INFO, "Server") << "POST /api/v1/audio/transcriptions" << std::endl;

        // OpenAI audio API uses multipart form data
        if (!req.is_multipart_form_data()) {
            res.status = 400;
            nlohmann::json error = {{"error", {
                {"message", "Request must be multipart/form-data"},
                {"type", "invalid_request_error"}
            }}};
            res.set_content(error.dump(), "application/json");
            return;
        }

        // Build request JSON for router
        nlohmann::json request_json = nlohmann::json::object();

        // Extract form fields
        if (req.form.has_field("model")) {
            request_json["model"] = req.form.get_field("model");
        }
        normalize_client_model_name(request_json);
        normalize_and_resolve_request_model(request_json);
        if (req.form.has_field("language")) {
            request_json["language"] = req.form.get_field("language");
        }
        if (req.form.has_field("prompt")) {
            request_json["prompt"] = req.form.get_field("prompt");
        }
        if (req.form.has_field("response_format")) {
            request_json["response_format"] = req.form.get_field("response_format");
        }
        if (req.form.has_field("temperature")) {
            request_json["temperature"] = std::stod(req.form.get_field("temperature"));
        }

        // Validate response format early
        std::string response_format =
            request_json.value("response_format", lemon::audio::ResponseFormat::JSON);

        if (!lemon::audio::is_supported_response_format(response_format)) {
            res.status = 400;
            nlohmann::json error = {{"error", {
                {"message", "Unsupported response_format: " + response_format},
                {"type", "invalid_request_error"}
            }}};
            res.set_content(error.dump(), "application/json");
            return;
        }

        // Extract audio file
        const auto& files = req.form.files;
        bool found_audio = false;
        for (const auto& file_pair : files) {
            if (file_pair.first == "file") {
                const auto& file = file_pair.second;
                request_json["file_data"] = file.content;
                request_json["filename"] = file.filename;
                found_audio = true;
                LOG(INFO, "Server") << "Audio file: " << file.filename
                          << " (" << file.content.size() << " bytes)" << std::endl;
                break;
            }
        }

        if (!found_audio) {
            res.status = 400;
            nlohmann::json error = {{"error", {
                {"message", "Missing 'file' field in request"},
                {"type", "invalid_request_error"}
            }}};
            res.set_content(error.dump(), "application/json");
            return;
        }

        // Handle model loading
        if (request_json.contains("model")) {
            std::string requested_model = request_json["model"];
            try {
                auto_load_model_if_needed(requested_model, extract_auto_load_options(request_json));
            } catch (const std::exception& e) {
                LOG(ERROR, "Server") << "Failed to load audio model: " << e.what() << std::endl;
                auto error_response = create_model_error(requested_model, e.what());
                std::string error_code = error_response["error"]["code"].get<std::string>();
                res.status = get_http_status_from_error(error_code);
                res.set_content(error_response.dump(), "application/json");
                return;
            }
        } else {
            res.status = 400;
            nlohmann::json error = {{"error", {
                {"message", "Missing 'model' field in request"},
                {"type", "invalid_request_error"}
            }}};
            res.set_content(error.dump(), "application/json");
            return;
        }

        // Forward to router
        auto response = router_->audio_transcriptions(request_json);

        if (response.contains("error")) {
            set_error_response(response, res, 500);
            return;
        }

        if (lemon::audio::is_plain_text_format(response_format) &&
            response.contains("text") &&
            response["text"].is_string()) {
            res.set_content(response["text"].get<std::string>(), "text/plain");
            return;
        }

        res.set_content(response.dump(), "application/json");

    } catch (const std::exception& e) {
        LOG(ERROR, "Server") << "ERROR in handle_audio_transcriptions: " << e.what() << std::endl;
        res.status = 500;
        nlohmann::json error = {{"error", {
            {"message", e.what()},
            {"type", "internal_error"}
        }}};
        res.set_content(error.dump(), "application/json");
    }
}

void Server::handle_audio_speech(const httplib::Request& req, httplib::Response& res) {
    try {
        auto request_json = nlohmann::json::parse(req.body);
        normalize_client_model_name(request_json);
        normalize_and_resolve_request_model(request_json);

        // Handle model loading
        if (request_json.contains("model")) {
            std::string requested_model = request_json["model"];
            if (auto info = router_->try_get_model_info(requested_model);
                info && info->type != ModelType::TTS) {
                res.status = 400;
                res.set_content(nlohmann::json{{"error", {
                    {"message", "Model '" + requested_model +
                        "' is not a text-to-speech model"},
                    {"type", "invalid_request_error"},
                    {"code", "model_not_applicable"}}}}.dump(),
                    "application/json");
                return;
            }
            try {
                auto_load_model_if_needed(requested_model, extract_auto_load_options(request_json));
            } catch (const std::exception& e) {
                LOG(ERROR, "Server") << "Failed to load text-to-speech model: " << e.what() << std::endl;
                auto error_response = create_model_error(requested_model, e.what());
                std::string error_code = error_response["error"]["code"].get<std::string>();
                res.status = get_http_status_from_error(error_code);
                res.set_content(error_response.dump(), "application/json");
                return;
            }
        } else {
            res.status = 400;
            nlohmann::json error = {{"error", {
                {"message", "Missing 'model' field in request"},
                {"type", "invalid_request_error"}
            }}};
            res.set_content(error.dump(), "application/json");
            return;
        }

        if (!request_json.contains("input")) {
            res.status = 400;
            nlohmann::json error = {{"error", {
                {"message", "Missing 'input' field in request"},
                {"type", "invalid_request_error"}
            }}};
            res.set_content(error.dump(), "application/json");
            return;
        }
        if (!request_json["input"].is_string()) {
            res.status = 400;
            nlohmann::json error = {{"error", {
                {"message", "'input' must be a string"},
                {"type", "invalid_request_error"}
            }}};
            res.set_content(error.dump(), "application/json");
            return;
        }

        bool is_streaming = (request_json.contains("stream") && request_json["stream"].get<bool>());

        if (request_json.contains("stream_format")) {
            is_streaming = true;
            if (request_json["stream_format"] != "audio") {
                res.status = 400;
                nlohmann::json error = {{"error", {
                    {"message", "Only 'audio' is supported for stream_format"},
                    {"type", "invalid_request_error"}
                }}};
                res.set_content(error.dump(), "application/json");
                return;
            }
        }

        const std::string speech_model = request_json["model"].get<std::string>();
        // A backend's streaming encoder is often narrower than its buffered one, so
        // which constraint applies depends on the transport in play.
        auto supported_formats = router_->audio_speech_supported_formats(speech_model);
        if (is_streaming) {
            auto streaming_formats = router_->audio_speech_supported_streaming_formats(speech_model);
            if (!streaming_formats.empty()) {
                supported_formats = std::move(streaming_formats);
            }
        }
        // Streaming and encoding are orthogonal: stream_format picks the transport,
        // response_format the container. An explicit response_format therefore wins
        // even when streaming, and is rejected below rather than silently swapped if
        // the backend can't emit it. Only the implicit default differs by transport,
        // and it yields to whatever the backend declares.
        std::string response_format;
        if (request_json.contains("response_format") && request_json["response_format"].is_string()) {
            response_format = request_json["response_format"].get<std::string>();
        } else {
            response_format = is_streaming ? "pcm" : "mp3";
            const bool default_supported = supported_formats.empty() ||
                std::find(supported_formats.begin(), supported_formats.end(),
                          response_format) != supported_formats.end();
            if (!default_supported) {
                response_format = supported_formats.front();
            }
        }
        if (!MIME_TYPES.contains(response_format)) {
            res.status = 400;
            nlohmann::json error = {{"error", {
                {"message", "Unsupported audio format requested"},
                {"type", "invalid_request_error"}
            }}};
            res.set_content(error.dump(), "application/json");
            return;
        }
        const bool format_supported = supported_formats.empty() ||
            std::find(supported_formats.begin(), supported_formats.end(),
                      response_format) != supported_formats.end();
        // TODO: transcode from a natively supported format instead of rejecting.
        if (!format_supported) {
            std::string supported_list;
            for (const auto& f : supported_formats) {
                supported_list += (supported_list.empty() ? "" : ", ") + f;
            }
            res.status = 400;
            nlohmann::json error = {{"error", {
                {"message", "response_format '" + response_format + "' is not supported by this model "
                            "(supported: " + supported_list + ")"},
                {"type", "invalid_request_error"}
            }}};
            res.set_content(error.dump(), "application/json");
            return;
        }
        const auto format_metadata =
            router_->audio_speech_format_metadata(speech_model, response_format);
        std::string mime_type = format_metadata.content_type.empty()
            ? MIME_TYPES[response_format].get<std::string>()
            : format_metadata.content_type;
        for (const auto& [name, value] : format_metadata.headers) {
            res.set_header(name, value);
        }
        // The backend has to encode what the Content-Type above promises. Without
        // this it would receive whatever the client sent -- nothing, when the
        // format came from a default -- and fall back to its own choice.
        request_json["response_format"] = response_format;

        // Log the HTTP request
        LOG(INFO, "Server") << "POST /api/v1/audio/speech" << std::endl;

        auto audio_source = [this, request_json](size_t offset, httplib::DataSink& sink) {
            // For chunked responses, offset tracks bytes sent so far
            // We only want to stream once when offset is 0
            if (offset > 0) {
                return false; // We're done after the first call
            }

            // Use unified Router path for streaming
            router_->audio_speech(request_json, sink);

            return false;
        };

        if (is_streaming) {
            res.set_header("Cache-Control", "no-cache");
            res.set_header("Connection", "keep-alive");
            res.set_header("X-Accel-Buffering", "no"); // Disable nginx buffering

            // Use cpp-httplib's chunked content provider for streaming
            res.set_chunked_content_provider(mime_type, audio_source);
        } else {
            serve_media_or_error(res, mime_type, [this, request_json](httplib::DataSink& sink) {
                router_->audio_speech(request_json, sink);
            });
        }

        return;
    } catch (const std::exception& e) {
        LOG(ERROR, "Server") << "ERROR in handle_audio_speech: " << e.what() << std::endl;
        res.status = 500;
        nlohmann::json error = {{"error", {
            {"message", e.what()},
            {"type", "internal_error"}
        }}};
        res.set_content(error.dump(), "application/json");
    }
}

// The streaming plumbing reports backend failures as a payload in the sink —
// either an SSE-style "data: {\"error\":...}" event or the backend's own JSON
// error body — rather than throwing. Returns the parsed error, or null.
static nlohmann::json extract_error_payload(const std::string& buf) {
    std::string body = buf;
    if (body.rfind("data: ", 0) == 0) {
        body = body.substr(6);
    }
    const auto start = body.find_first_not_of(" \t\r\n");
    if (start == std::string::npos || body[start] != '{') {
        return nullptr;
    }
    try {
        auto parsed = nlohmann::json::parse(body.substr(start));
        if (parsed.is_object() && parsed.contains("error")) {
            return parsed;
        }
    } catch (...) {
    }
    return nullptr;
}

void Server::serve_media_or_error(httplib::Response& res, const std::string& mime_type,
                                  const std::function<void(httplib::DataSink&)>& generate) {
    // Buffer the generated media so we can tell success from a silent failure. A
    // streaming content provider commits a 200 before the backend runs, so a backend
    // crash/OOM mid-stream would surface as a successful empty file; buffering lets us
    // return a real error instead.
    std::string buf;
    httplib::DataSink sink;
    sink.write = [&buf](const char* data, size_t len) { buf.append(data, len); return true; };
    sink.is_writable = []() { return true; };
    sink.done = []() {};
    generate(sink);
    if (buf.empty()) {
        res.status = 502;
        res.set_content(nlohmann::json{{"error", {
            {"message", "Generation failed: the backend produced no output (it likely crashed or ran "
                        "out of GPU memory). Check the server logs."},
            {"type", "backend_error"}}}}.dump(), "application/json");
        return;
    }
    if (auto error_payload = extract_error_payload(buf); !error_payload.is_null()) {
        res.status = get_error_status_code(error_payload, 500);
        res.set_content(error_payload.dump(), "application/json");
        return;
    }
    res.set_content(buf, mime_type);
}

void Server::handle_audio_generations(const httplib::Request& req, httplib::Response& res) {
    try {
        auto request_json = nlohmann::json::parse(req.body);

        if (!request_json.contains("model")) {
            res.status = 400;
            res.set_content(nlohmann::json{{"error", {
                {"message", "Missing 'model' field in request"},
                {"type", "invalid_request_error"}}}}.dump(), "application/json");
            return;
        }
        if (!request_json.contains("prompt")) {
            res.status = 400;
            res.set_content(nlohmann::json{{"error", {
                {"message", "Missing 'prompt' field in request"},
                {"type", "invalid_request_error"}}}}.dump(), "application/json");
            return;
        }
        for (const auto* field : {"prompt", "lyrics", "vocal_language"}) {
            if (request_json.contains(field) && !request_json[field].is_string()) {
                res.status = 400;
                res.set_content(nlohmann::json{{"error", {
                    {"message", "'" + std::string(field) + "' must be a string"},
                    {"type", "invalid_request_error"}}}}.dump(), "application/json");
                return;
            }
        }

        std::string requested_model = request_json["model"];
        if (auto info = router_->try_get_model_info(requested_model);
            info && info->type != ModelType::AUDIO_GENERATION) {
            res.status = 400;
            res.set_content(nlohmann::json{{"error", {
                {"message", "Model '" + requested_model +
                    "' is not an audio-generation model"},
                {"type", "invalid_request_error"},
                {"code", "model_not_applicable"}}}}.dump(),
                "application/json");
            return;
        }
        try {
            auto_load_model_if_needed(requested_model, extract_auto_load_options(request_json));
        } catch (const std::exception& e) {
            LOG(ERROR, "Server") << "Failed to load audio-generation model: " << e.what() << std::endl;
            auto error_response = create_model_error(requested_model, e.what());
            res.status = get_http_status_from_error(error_response["error"]["code"].get<std::string>());
            res.set_content(error_response.dump(), "application/json");
            return;
        }

        std::string response_format = "wav";
        if (request_json.contains("response_format") && request_json["response_format"].is_string()) {
            response_format = request_json["response_format"].get<std::string>();
        }
        const auto supported_formats = router_->audio_generation_supported_formats(requested_model);
        const bool format_supported = std::find(supported_formats.begin(), supported_formats.end(),
                                                response_format) != supported_formats.end();
        // TODO: transcode from a natively supported format instead of rejecting.
        if (!supported_formats.empty() && !format_supported) {
            std::string supported_list;
            for (const auto& f : supported_formats) {
                supported_list += (supported_list.empty() ? "" : ", ") + f;
            }
            res.status = 400;
            res.set_content(nlohmann::json{{"error", {
                {"message", "response_format '" + response_format + "' is not supported by this model "
                            "(supported: " + supported_list + ")"},
                {"type", "invalid_request_error"}}}}.dump(), "application/json");
            return;
        }
        const auto format_metadata =
            router_->audio_generation_format_metadata(requested_model, response_format);
        std::string mime_type = !format_metadata.content_type.empty()
            ? format_metadata.content_type
            : (MIME_TYPES.contains(response_format)
                ? MIME_TYPES[response_format].get<std::string>()
                : MIME_TYPES["wav"].get<std::string>());
        for (const auto& [name, value] : format_metadata.headers) {
            res.set_header(name, value);
        }

        LOG(INFO, "Server") << "POST /api/v1/audio/generations" << std::endl;

        serve_media_or_error(res, mime_type, [this, request_json](httplib::DataSink& sink) {
            router_->audio_generations(request_json, sink);
        });
    } catch (const std::exception& e) {
        LOG(ERROR, "Server") << "ERROR in handle_audio_generations: " << e.what() << std::endl;
        res.status = 500;
        res.set_content(nlohmann::json{{"error", {
            {"message", e.what()}, {"type", "internal_error"}}}}.dump(), "application/json");
    }
}

void Server::handle_3d_generations(const httplib::Request& req, httplib::Response& res) {
    try {
        auto request_json = nlohmann::json::parse(req.body);

        // All cheap validation runs before auto_load_model_if_needed so a
        // malformed request can never trigger a multi-gigabyte model load.
        auto reject = [&res](const std::string& message) {
            res.status = 400;
            res.set_content(nlohmann::json{{"error", {
                {"message", message},
                {"type", "invalid_request_error"}}}}.dump(), "application/json");
        };
        if (!request_json.contains("model") || !request_json["model"].is_string()) {
            return reject("Missing or non-string 'model' field in request");
        }
        if (!request_json.contains("image") || !request_json["image"].is_string()) {
            return reject("Missing or non-string 'image' field in request (base64-encoded input image)");
        }
        {
            std::string image = request_json["image"].get<std::string>();
            if (image.rfind("data:", 0) == 0) {
                const auto comma = image.find(',');
                if (comma == std::string::npos) {
                    return reject("'image' data URL is missing its base64 payload");
                }
                image = image.substr(comma + 1);
            }
            if (image.empty() || image.find_first_not_of(
                    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=\r\n") !=
                    std::string::npos) {
                return reject("'image' must be base64 data or a base64 data: URL");
            }
            // Sniffing needs only the first 12 bytes; base64 is block-aligned,
            // so decoding the first 16 characters is enough and avoids decoding
            // a multi-megabyte payload just to validate it.
            std::string head;
            for (char c : image) {
                if (c != '\r' && c != '\n') {
                    head += c;
                    if (head.size() == 16) break;
                }
            }
            if (!utils::sniff_image(utils::JsonUtils::base64_decode(head)).ok()) {
                return reject("'image' is not a supported format (expected PNG, JPEG, BMP, or GIF)");
            }
        }
        std::string response_format = "glb";
        if (request_json.contains("response_format")) {
            if (!request_json["response_format"].is_string()) {
                return reject("'response_format' must be a string");
            }
            response_format = request_json["response_format"].get<std::string>();
        }
        // TODO: convert from a natively supported format instead of rejecting.
        if (response_format != "glb") {
            return reject("response_format '" + response_format +
                          "' is not supported (supported: glb)");
        }
        if (request_json.contains("resolution")) {
            const auto& r = request_json["resolution"];
            const std::string v = r.is_string() ? r.get<std::string>()
                : (r.is_number_integer() ? std::to_string(r.get<int>()) : "");
            if (v != "512" && v != "1024" && v != "1536") {
                return reject("'resolution' must be 512, 1024, or 1536");
            }
        }
        if (request_json.contains("bg_removal")) {
            const auto& b = request_json["bg_removal"];
            if (!b.is_string() || (b != "threshold" && b != "birefnet")) {
                return reject("'bg_removal' must be 'threshold' or 'birefnet'");
            }
        }
        if (request_json.contains("seed") && !request_json["seed"].is_number_integer()) {
            return reject("'seed' must be an integer");
        }
        if (request_json.contains("uv")) {
            const auto& u = request_json["uv"];
            if (!u.is_string() || (u != "box" && u != "xatlas")) {
                return reject("'uv' must be 'box' or 'xatlas'");
            }
        }

        std::string requested_model = request_json["model"];
        try {
            auto_load_model_if_needed(requested_model, extract_auto_load_options(request_json));
        } catch (const std::exception& e) {
            LOG(ERROR, "Server") << "Failed to load 3D-generation model: " << e.what() << std::endl;
            auto error_response = create_model_error(requested_model, e.what());
            res.status = get_http_status_from_error(error_response["error"]["code"].get<std::string>());
            res.set_content(error_response.dump(), "application/json");
            return;
        }

        LOG(INFO, "Server") << "POST /api/v1/3d/generations" << std::endl;

        serve_media_or_error(res, "model/gltf-binary", [this, request_json](httplib::DataSink& sink) {
            router_->model_3d_generations(request_json, sink);
        });
    } catch (const std::exception& e) {
        LOG(ERROR, "Server") << "ERROR in handle_3d_generations: " << e.what() << std::endl;
        res.status = 500;
        res.set_content(nlohmann::json{{"error", {
            {"message", e.what()}, {"type", "internal_error"}}}}.dump(), "application/json");
    }
}

void Server::handle_image_generations(const httplib::Request& req, httplib::Response& res) {
    try {
        LOG(INFO, "Server") << "POST /api/v1/images/generations" << std::endl;

        auto request_json = nlohmann::json::parse(req.body);
        normalize_client_model_name(request_json);
        normalize_and_resolve_request_model(request_json);

        // Validate required fields
        if (!request_json.contains("prompt")) {
            res.status = 400;
            nlohmann::json error = {{"error", {
                {"message", "Missing 'prompt' field in request"},
                {"type", "invalid_request_error"}
            }}};
            res.set_content(error.dump(), "application/json");
            return;
        }

        if (!request_json.contains("model")) {
            res.status = 400;
            nlohmann::json error = {{"error", {
                {"message", "Missing 'model' field in request"},
                {"type", "invalid_request_error"}
            }}};
            res.set_content(error.dump(), "application/json");
            return;
        }

        std::string requested_model = request_json["model"];

        try {
            auto_load_model_if_needed(requested_model, extract_auto_load_options(request_json));
        } catch (const std::exception& e) {
            LOG(ERROR, "Server") << "Failed to load image model: " << e.what() << std::endl;
            auto error_response = create_model_error(requested_model, e.what());
            std::string error_code = error_response["error"]["code"].get<std::string>();
            res.status = get_http_status_from_error(error_code);
            res.set_content(error_response.dump(), "application/json");
            return;
        }

        {
            auto response = router_->image_generations(request_json);
            if (response.contains("error")) {
                LOG(ERROR, "Server") << "Image generation backend error: " << response.dump() << std::endl;
                res.status = 500;
            }
            res.set_content(response.dump(), "application/json");
        }

    } catch (const nlohmann::json::exception& e) {
        LOG(ERROR, "Server") << "JSON parse error in handle_image_generations: " << e.what() << std::endl;
        res.status = 400;
        nlohmann::json error = {{"error", {
            {"message", "Invalid JSON: " + std::string(e.what())},
            {"type", "invalid_request_error"}
        }}};
        res.set_content(error.dump(), "application/json");
    } catch (const std::exception& e) {
        LOG(ERROR, "Server") << "ERROR in handle_image_generations: " << e.what() << std::endl;
        res.status = 500;
        nlohmann::json error = {{"error", {
            {"message", e.what()},
            {"type", "internal_error"}
        }}};
        res.set_content(error.dump(), "application/json");
    }
}

bool Server::parse_n_from_form(const httplib::Request& req, httplib::Response& res, nlohmann::json& out) {
    if (!req.form.has_field("n")) {
        return true;
    }
    int n;
    try {
        size_t pos;
        const std::string& val = req.form.get_field("n");
        n = std::stoi(val, &pos);
        if (pos != val.size()) throw std::invalid_argument("trailing characters");
    } catch (const std::exception&) {
        res.status = 400;
        nlohmann::json error = {{"error", {
            {"message", "Invalid value for 'n': must be an integer"},
            {"type", "invalid_request_error"}
        }}};
        res.set_content(error.dump(), "application/json");
        return false;
    }
    if (n < 1 || n > 10) {
        res.status = 400;
        nlohmann::json error = {{"error", {
            {"message", "Invalid value for 'n': must be between 1 and 10"},
            {"type", "invalid_request_error"}
        }}};
        res.set_content(error.dump(), "application/json");
        return false;
    }
    out["n"] = n;
    return true;
}

bool Server::extract_image_from_form(const httplib::Request& req, httplib::Response& res, nlohmann::json& out, bool allow_multiple) {
    // A multipart map groups files by field name, losing mixed upload order.
    if (allow_multiple && req.form.files.count("image") > 0 &&
        req.form.files.count("image[]") > 0) {
        res.status = 400;
        nlohmann::json error = {{"error", {
            {"message", "Use either 'image' or 'image[]', not both"},
            {"type", "invalid_request_error"}
        }}};
        res.set_content(error.dump(), "application/json");
        return false;
    }
    nlohmann::json images = nlohmann::json::array();
    for (const auto& file_pair : req.form.files) {
        if (file_pair.first == "image" || file_pair.first == "image[]") {
            const auto& file = file_pair.second;
            const std::string encoded = utils::JsonUtils::base64_encode(file.content);
            if (!out.contains("image_data")) {
                // Keep the existing single-image contract for other callers.
                out["image_data"] = encoded;
                out["image_filename"] = file.filename;
                out["image_content_type"] = file.content_type;
            }
            LOG(INFO, "Server") << "Image file: " << file.filename
                      << " (" << file.content.size() << " bytes)" << std::endl;
            if (!allow_multiple) return true;
            // Preserve original upload bytes, MIME and reference order.
            images.push_back({
                {"data", encoded},
                {"filename", file.filename},
                {"content_type", file.content_type},
            });
        }
    }
    if (!images.empty()) {
        out["image_files"] = std::move(images);
        return true;
    }
    res.status = 400;
    nlohmann::json error = {{"error", {
        {"message", "Missing 'image' field in request"},
        {"type", "invalid_request_error"}
    }}};
    res.set_content(error.dump(), "application/json");
    return false;
}

bool Server::load_image_model(const nlohmann::json& request_json, httplib::Response& res) {
    if (!request_json.contains("model")) {
        res.status = 400;
        nlohmann::json error = {{"error", {
            {"message", "Missing 'model' field in request"},
            {"type", "invalid_request_error"}
        }}};
        res.set_content(error.dump(), "application/json");
        return false;
    }
    std::string requested_model = request_json["model"];
    try {
        auto_load_model_if_needed(requested_model, extract_auto_load_options(request_json));
    } catch (const std::exception& e) {
        LOG(ERROR, "Server") << "Failed to load image model: " << e.what() << std::endl;
        auto error_response = create_model_error(requested_model, e.what());
        std::string error_code = error_response["error"]["code"].get<std::string>();
        res.status = get_http_status_from_error(error_code);
        res.set_content(error_response.dump(), "application/json");
        return false;
    }
    return true;
}

void Server::handle_image_edits(const httplib::Request& req, httplib::Response& res) {
    try {
        LOG(INFO, "Server") << "POST /api/v1/images/edits" << std::endl;

        if (!req.is_multipart_form_data()) {
            res.status = 400;
            nlohmann::json error = {{"error", {
                {"message", "Request must be multipart/form-data"},
                {"type", "invalid_request_error"}
            }}};
            res.set_content(error.dump(), "application/json");
            return;
        }

        nlohmann::json request_json;

        // Extract common form fields
        if (req.form.has_field("model"))            request_json["model"]            = req.form.get_field("model");
        if (req.form.has_field("prompt"))           request_json["prompt"]           = req.form.get_field("prompt");
        if (req.form.has_field("size"))             request_json["size"]             = req.form.get_field("size");
        if (req.form.has_field("response_format"))  request_json["response_format"]  = req.form.get_field("response_format");
        if (req.form.has_field("user"))             request_json["user"]             = req.form.get_field("user");
        if (req.form.has_field("background"))       request_json["background"]       = req.form.get_field("background");
        if (req.form.has_field("quality"))          request_json["quality"]          = req.form.get_field("quality");
        if (req.form.has_field("input_fidelity"))   request_json["input_fidelity"]   = req.form.get_field("input_fidelity");

        if (req.form.has_field("output_compression")) {
            int output_compression;
            try {
                size_t pos;
                const std::string& val = req.form.get_field("output_compression");
                output_compression = std::stoi(val, &pos);
                if (pos != val.size()) throw std::invalid_argument("trailing characters");
            } catch (const std::exception&) {
                res.status = 400;
                nlohmann::json error = {{"error", {
                    {"message", "Invalid value for 'output_compression': must be an integer"},
                    {"type", "invalid_request_error"}
                }}};
                res.set_content(error.dump(), "application/json");
                return;
            }
            request_json["output_compression"] = output_compression;
        }

        // Extract optional numeric inference parameters
        auto parse_int_field = [&](const std::string& field) -> bool {
            if (!req.form.has_field(field)) return true;
            const std::string& val = req.form.get_field(field);
            try {
                size_t pos;
                int parsed = std::stoi(val, &pos);
                if (pos != val.size()) throw std::invalid_argument("trailing characters");
                request_json[field] = parsed;
            } catch (const std::exception&) {
                res.status = 400;
                nlohmann::json error = {{"error", {
                    {"message", "Invalid value for '" + field + "': must be an integer"},
                    {"type", "invalid_request_error"}
                }}};
                res.set_content(error.dump(), "application/json");
                return false;
            }
            return true;
        };
        auto parse_float_field = [&](const std::string& field) -> bool {
            if (!req.form.has_field(field)) return true;
            const std::string& val = req.form.get_field(field);
            try {
                size_t pos;
                float parsed = std::stof(val, &pos);
                if (pos != val.size()) throw std::invalid_argument("trailing characters");
                if (std::isnan(parsed) || std::isinf(parsed)) throw std::invalid_argument("nan/inf not allowed");
                request_json[field] = parsed;
            } catch (const std::exception&) {
                res.status = 400;
                nlohmann::json error = {{"error", {
                    {"message", "Invalid value for '" + field + "': must be a number"},
                    {"type", "invalid_request_error"}
                }}};
                res.set_content(error.dump(), "application/json");
                return false;
            }
            return true;
        };
        if (!parse_int_field("steps"))     return;
        if (!parse_float_field("cfg_scale")) return;
        if (!parse_int_field("seed"))      return;

        if (!parse_n_from_form(req, res, request_json))      return;
        if (!extract_image_from_form(req, res, request_json, /*allow_multiple=*/true)) return;

        // The first uploaded image sets the output size unless explicitly provided.
        if (!request_json.contains("size")) {
            std::uint64_t soft_pixel_cap = kDefaultMaxEditPixels;
            if (request_json.contains("model") && request_json["model"].is_string()) {
                const auto model = router_->try_get_model_info(
                    resolve_alias_target(request_json["model"].get<std::string>()));
                if (model && model->image_defaults.soft_pixel_cap > 0) {
                    soft_pixel_cap = model->image_defaults.soft_pixel_cap;
                }
            }
            for (const auto& file_pair : req.form.files) {
                if (file_pair.first != "image" && file_pair.first != "image[]") continue;
                const auto original = image_dimensions_from_bytes(file_pair.second.content);
                if (original) {
                    const auto [width, height] = cap_image_area(
                        original->first, original->second, soft_pixel_cap);
                    request_json["size"] = std::to_string(width) + "x" + std::to_string(height);
                } else {
                    LOG(WARNING, "Server") << "Cannot determine source image dimensions; "
                        "using backend defaults" << std::endl;
                }
                break;
            }
        }

        // Extract optional mask file
        for (const auto& file_pair : req.form.files) {
            if (file_pair.first == "mask") {
                const auto& file = file_pair.second;
                request_json["mask_data"] = utils::JsonUtils::base64_encode(file.content);
                request_json["mask_filename"] = file.filename;
                LOG(INFO, "Server") << "Mask file: " << file.filename
                          << " (" << file.content.size() << " bytes)" << std::endl;
                break;
            }
        }

        if (!request_json.contains("prompt")) {
            res.status = 400;
            nlohmann::json error = {{"error", {
                {"message", "Missing 'prompt' field in request"},
                {"type", "invalid_request_error"}
            }}};
            res.set_content(error.dump(), "application/json");
            return;
        }

        if (!load_image_model(request_json, res)) return;

        auto response = router_->image_edits(request_json);
        if (response.contains("error")) {
            LOG(ERROR, "Server") << "Image edits backend error: " << response.dump() << std::endl;
            res.status = 500;
        }
        res.set_content(response.dump(), "application/json");

    } catch (const nlohmann::json::exception& e) {
        LOG(ERROR, "Server") << "JSON parse error in handle_image_edits: " << e.what() << std::endl;
        res.status = 400;
        nlohmann::json error = {{"error", {
            {"message", "Invalid JSON: " + std::string(e.what())},
            {"type", "invalid_request_error"}
        }}};
        res.set_content(error.dump(), "application/json");
    } catch (const std::exception& e) {
        LOG(ERROR, "Server") << "ERROR in handle_image_edits: " << e.what() << std::endl;
        res.status = 500;
        nlohmann::json error = {{"error", {
            {"message", e.what()},
            {"type", "server_error"}
        }}};
        res.set_content(error.dump(), "application/json");
    }
}

void Server::handle_image_variations(const httplib::Request& req, httplib::Response& res) {
    try {
        LOG(INFO, "Server") << "POST /api/v1/images/variations" << std::endl;

        if (!req.is_multipart_form_data()) {
            res.status = 400;
            nlohmann::json error = {{"error", {
                {"message", "Request must be multipart/form-data"},
                {"type", "invalid_request_error"}
            }}};
            res.set_content(error.dump(), "application/json");
            return;
        }

        nlohmann::json request_json;

        // Extract common form fields
        if (req.form.has_field("model"))            request_json["model"]            = req.form.get_field("model");
        if (req.form.has_field("size"))             request_json["size"]             = req.form.get_field("size");
        if (req.form.has_field("response_format"))  request_json["response_format"]  = req.form.get_field("response_format");
        if (req.form.has_field("user"))             request_json["user"]             = req.form.get_field("user");

        if (!parse_n_from_form(req, res, request_json))      return;
        if (!extract_image_from_form(req, res, request_json)) return;
        if (!load_image_model(request_json, res))             return;

        auto response = router_->image_variations(request_json);
        if (response.contains("error")) {
            LOG(ERROR, "Server") << "Image variations backend error: " << response.dump() << std::endl;
            res.status = 500;
        }
        res.set_content(response.dump(), "application/json");

    } catch (const nlohmann::json::exception& e) {
        LOG(ERROR, "Server") << "JSON parse error in handle_image_variations: " << e.what() << std::endl;
        res.status = 400;
        nlohmann::json error = {{"error", {
            {"message", "Invalid JSON: " + std::string(e.what())},
            {"type", "invalid_request_error"}
        }}};
        res.set_content(error.dump(), "application/json");
    } catch (const std::exception& e) {
        LOG(ERROR, "Server") << "ERROR in handle_image_variations: " << e.what() << std::endl;
        res.status = 500;
        nlohmann::json error = {{"error", {
            {"message", e.what()},
            {"type", "server_error"}
        }}};
        res.set_content(error.dump(), "application/json");
    }
}

void Server::handle_image_upscale(const httplib::Request& req, httplib::Response& res) {
    try {
        LOG(INFO, "Server") << "POST /api/v1/images/upscale" << std::endl;

        auto request_json = nlohmann::json::parse(req.body);

        if (!request_json.contains("image") || !request_json["image"].is_string()) {
            res.status = 400;
            nlohmann::json error = {{"error", {
                {"message", "Missing 'image' field (base64 encoded)"},
                {"type", "invalid_request_error"}
            }}};
            res.set_content(error.dump(), "application/json");
            return;
        }

        std::string upscale_model_name = request_json.value("model", "");
        if (upscale_model_name.empty()) {
            res.status = 400;
            nlohmann::json error = {{"error", {
                {"message", "Missing 'model' field"},
                {"type", "invalid_request_error"}
            }}};
            res.set_content(error.dump(), "application/json");
            return;
        }

        ModelInfo info;
        try {
            info = model_manager_->get_model_info(upscale_model_name);

            if (!lemon::has_label(info.labels, "upscaling")) {
                res.status = 400;
                nlohmann::json error = {{"error", {
                    {"message", "Upscale model is not labeled 'upscaling': " + upscale_model_name},
                    {"type", "invalid_request_error"}
                }}};
                res.set_content(error.dump(), "application/json");
                return;
            }

            if (!model_manager_->is_model_downloaded(upscale_model_name)) {
                LOG(INFO, "Server") << "Upscale model not cached, downloading from its remote registry..." << std::endl;
                model_manager_->download_registered_model(info, true);
                LOG(INFO, "Server") << "Upscale model download complete: " << upscale_model_name << std::endl;
                info = model_manager_->get_model_info(upscale_model_name);
            }

        } catch (const std::exception& e) {
            res.status = 404;
            nlohmann::json error = {{"error", {
                {"message", "Upscale model not found: " + upscale_model_name},
                {"type", "invalid_request_error"}
            }}};
            res.set_content(error.dump(), "application/json");
            return;
        }

        std::string b64_image = request_json["image"].get<std::string>();

        backends::BackendContext context;
        context.log_level = config_->log_level();
        context.model_manager = model_manager_.get();
        context.backend_manager = backend_manager_.get();
        context.cloud_registry = cloud_registry_.get();
        context.model_info = &info;
        auto server = backends::create_server(info.recipe, context);
        if (!server || !supports_capability<IUpscaleServer>(server.get())) {
            res.status = 400;
            nlohmann::json error = {{"error", {
                {"message", "Upscale is not supported by recipe: " + info.recipe},
                {"type", "invalid_request_error"}
            }}};
            res.set_content(error.dump(), "application/json");
            return;
        }
        auto* upscale_server = dynamic_cast<IUpscaleServer*>(server.get());
        std::string upscaled = upscale_server->upscale_via_cli(b64_image, info.resolved_path("main"));

        if (upscaled.empty()) {
            res.status = 500;
            nlohmann::json error = {{"error", {
                {"message", "Upscale failed"},
                {"type", "server_error"}
            }}};
            res.set_content(error.dump(), "application/json");
            return;
        }

        nlohmann::json response;
        response["created"] = static_cast<long long>(std::time(nullptr));
        response["data"] = nlohmann::json::array();
        response["data"].push_back({{"b64_json", upscaled}});
        res.set_content(response.dump(), "application/json");

    } catch (const nlohmann::json::exception& e) {
        LOG(ERROR, "Server") << "JSON parse error in handle_image_upscale: " << e.what() << std::endl;
        res.status = 400;
        nlohmann::json error = {{"error", {
            {"message", "Invalid JSON: " + std::string(e.what())},
            {"type", "invalid_request_error"}
        }}};
        res.set_content(error.dump(), "application/json");
    } catch (const std::exception& e) {
        LOG(ERROR, "Server") << "ERROR in handle_image_upscale: " << e.what() << std::endl;
        res.status = 500;
        nlohmann::json error = {{"error", {
            {"message", e.what()},
            {"type", "server_error"}
        }}};
        res.set_content(error.dump(), "application/json");
    }
}

void Server::handle_responses(const httplib::Request& req, httplib::Response& res) {
    utils::RequestCancelToken cancel_token;
    if (req.is_connection_closed) {
        cancel_token.should_cancel = [is_closed = req.is_connection_closed]() { return is_closed && is_closed(); };
    }
    WrappedServer::RequestCancelScope cancel_scope(cancel_token);

    try {
        auto request_json = nlohmann::json::parse(req.body);
        normalize_client_model_name(request_json);
        normalize_and_resolve_request_model(request_json);

        // A collection.router model flips this endpoint into engine mode: pick a
        // candidate and rewrite the model before the usual load/forward logic.
        std::optional<RouterDispatchResult> route_dispatch =
            apply_router_collection_dispatch(request_json);

        // Handle model loading/switching using helper function
        if (request_json.contains("model")) {
            std::string requested_model = request_json["model"];
            try {
                auto_load_model_if_needed(requested_model, extract_auto_load_options(request_json));
            } catch (const std::exception& e) {
                LOG(ERROR, "Server") << "Failed to load model: " << e.what() << std::endl;
                auto error_response = create_model_error(requested_model, e.what());
                std::string error_code = error_response["error"]["code"].get<std::string>();
                res.status = get_http_status_from_error(error_code);
                res.set_content(error_response.dump(), "application/json");
                return;
            }
        } else if (!router_->is_model_loaded()) {
            LOG(ERROR, "Server") << "No model loaded and no model specified in request" << std::endl;
            res.status = 400;
            res.set_content("{\"error\": \"No model loaded and no model specified in request\"}", "application/json");
            return;
        }

        // Check if streaming is requested
        bool is_streaming = request_json.contains("stream") && request_json["stream"].get<bool>();

        if (is_streaming) {
            try {
                // Re-serialize so any collection.router model rewrite reaches the backend.
                std::string request_body = request_json.dump();
                LOG(INFO, "Server") << "POST /api/v1/responses - Streaming" << std::endl;

                set_route_decision_sse_content_provider(
                    res,
                    route_dispatch,
                    request_body,
                    [this](const std::string& body, httplib::DataSink& sink) {
                        router_->responses_stream(body, sink);
                    });
            } catch (const std::exception& e) {
                LOG(ERROR, "Server") << "Streaming failed: " << e.what() << std::endl;
                res.status = 500;
                res.set_content("{\"error\":\"Internal server error during streaming\"}", "application/json");
            }
        } else {
            LOG(INFO, "Server") << "POST /api/v1/responses - Non-streaming" << std::endl;

            auto response = router_->responses(request_json);

            if (response.contains("error")) {
                LOG(ERROR, "Server") << "Responses backend error: " << response["error"].dump() << std::endl;
                set_error_response(response, res);
                return;
            }

            attach_route_decision(response, res, route_dispatch);
            LOG(INFO, "Server") << "200 OK" << std::endl;
            res.set_content(response.dump(), "application/json");

            record_response_telemetry(response, request_json);
        }

    } catch (const RouterResidencyConflictException& e) {
        LOG(WARNING, "Server") << "Router residency conflict in responses: "
                               << e.what() << std::endl;
        set_router_residency_conflict_response(e, res);
    } catch (const std::exception& e) {
        LOG(ERROR, "Server") << "ERROR in handle_responses: " << e.what() << std::endl;
        res.status = 500;
        nlohmann::json error = {{"error", e.what()}};
        res.set_content(error.dump(), "application/json");
    }
}
bool Server::parse_required_json_body(const httplib::Request& req,
                                      httplib::Response& res,
                                      nlohmann::json& out) {
    if (req.body.empty()) {
        res.status = 400;
        nlohmann::json error = {{"error", "Request body is required but was empty"}};
        res.set_content(error.dump(), "application/json");
        return false;
    }
    try {
        out = nlohmann::json::parse(req.body);
        return true;
    } catch (const nlohmann::json::exception& e) {
        // Not just parse_error: a numeric literal too large for a double raises
        // out_of_range, which is a sibling type and would otherwise escape as a 500.
        res.status = 400;
        nlohmann::json error = {{"error", std::string("Invalid JSON in request body: ") + e.what()}};
        res.set_content(error.dump(), "application/json");
        return false;
    }
}

void Server::handle_pull(const httplib::Request& req, httplib::Response& res) {
    auto bad_request = [&res](const std::string& message) {
        res.status = 400;
        nlohmann::json error = {{"error", message}};
        res.set_content(error.dump(), "application/json");
    };

    nlohmann::json request_json;
    if (!parse_required_json_body(req, res, request_json)) return;

    try {
        // Accept both "model" and "model_name" for compatibility
        std::string model_name = request_json.contains("model") ?
            request_json["model"].get<std::string>() :
            request_json["model_name"].get<std::string>();

        // Extract optional parameters
        std::string checkpoint = request_json.value("checkpoint", "");
        std::string recipe = request_json.value("recipe", "");
        bool do_not_upgrade = request_json.value("do_not_upgrade", false);
        bool stream = request_json.value("stream", false);
        bool subscribe = request_json.value("subscribe", true);
        bool local_import = request_json.value("local_import", false);

        // Resolve the pull's registry provenance once, up front, so download,
        // cache layout, and later refresh stay consistent. A registry-backed
        // checkpoint that names no source inherits the configured default; a
        // provider URL is normalized to owner/repo and its registry adopted.
        // Self-managed backends (flm/cloud), non-registry checkpoints, explicit
        // sources, and bare `pull <registered-name>` refreshes are left as-is.
        try {
            lemon::apply_default_pull_source(request_json, config_->default_model_source());
        } catch (const std::invalid_argument& e) {
            bad_request(e.what());
            return;
        }
        // The helper may have rewritten a provider URL into an owner/repo id.
        checkpoint = request_json.value("checkpoint", "");

        LOG(INFO, "Server") << "Pulling model: " << model_name << std::endl;
        if (!checkpoint.empty()) {
            LOG(INFO, "Server") << "   checkpoint: " << checkpoint << std::endl;
        }
        if (!recipe.empty()) {
            LOG(INFO, "Server") << "   recipe: " << recipe << std::endl;
        }

        // Both API operations always enter the same registration path.
        // /pull continues with installation after this; /models/register returns.
        const bool collection_file_import =
            request_json.contains("models") && request_json["models"].is_array();

        try {
            register_model_definition_internal(
                model_name,
                request_json,
                /*require_definition=*/false,
                /*allow_embedded_models=*/true,
                local_import);
        } catch (const std::invalid_argument& e) {
            bad_request(e.what());
            return;
        }

        if (local_import) {
            nlohmann::json response = {
                {"status", "success"},
                {"model_name", model_name},
                {"message", "Model imported and registered successfully"}
            };
            res.set_content(response.dump(), "application/json");
            return;
        }

        if (config_->offline()) {
            res.status = 400;
            nlohmann::json error = {{"error", "Lemond is in offline mode, models not downloaded"}, {"code", "lemond_offline"}};
            res.set_content(error.dump(), "application/json");
            return;
        }

        nlohmann::json download_request = collection_file_import
            ? request_json
            : nlohmann::json::object();
        for (const char* field : {"source", "registry_source"}) {
            if (request_json.contains(field)) {
                download_request[field] = request_json[field];
            }
        }

        if (stream) {
            auto operation = [this, model_name, download_request, do_not_upgrade](DownloadProgressCallback progress_cb) {
                model_manager_->download_model(model_name, download_request, do_not_upgrade, progress_cb);
            };

            if (!subscribe) {
                // Server-owned mode for desktop UI reload/new-tab recovery.
                // Legacy streamed /pull behavior is unchanged.
                auto job = start_download_job("model:" + model_name, "model", model_name, operation);
                nlohmann::json response;
                {
                    // The worker can update the shared job immediately after
                    // start_download_job returns, so copy the response while
                    // holding the same mutex used by the progress callback.
                    std::lock_guard<std::mutex> lock(downloads_mutex_);
                    response = download_job_to_json(job);
                }
                res.set_content(response.dump(), "application/json");
                return;
            }

            // Backward-compatible SSE streaming mode: tie the operation to this
            // response exactly as before.
            stream_download_operation(res, operation);
        } else {
            // Legacy synchronous mode - blocks until complete
            model_manager_->download_model(model_name, download_request, do_not_upgrade);

            nlohmann::json response = {{"status", "success"}, {"model_name", model_name}};
            res.set_content(response.dump(), "application/json");
        }

    } catch (const lemon::UnknownModelError& e) {
        LOG(ERROR, "Server") << "ERROR in handle_pull: " << e.what() << std::endl;
        res.status = 400;
        nlohmann::json error = {{"error", e.what()}, {"code", lemon::kUnknownModelErrorCode}};
        res.set_content(error.dump(), "application/json");
    } catch (const lemon::InvalidModelDefinitionError& e) {
        LOG(ERROR, "Server") << "ERROR in handle_pull: " << e.what() << std::endl;
        res.status = 400;
        nlohmann::json error = {{"error", e.what()}};
        res.set_content(error.dump(), "application/json");
    } catch (const std::exception& e) {
        LOG(ERROR, "Server") << "ERROR in handle_pull: " << e.what() << std::endl;
        res.status = 500;
        nlohmann::json error = {{"error", e.what()}};
        res.set_content(error.dump(), "application/json");
    }
}

void Server::handle_registry_search(const httplib::Request& req, httplib::Response& res) {
    try {
        std::string query = req.has_param("query")
            ? req.get_param_value("query")
            : (req.has_param("q") ? req.get_param_value("q") : "");
        const auto first = query.find_first_not_of(" \t\r\n");
        const auto last = query.find_last_not_of(" \t\r\n");
        query = first == std::string::npos ? "" : query.substr(first, last - first + 1);
        if (query.size() < 3) {
            res.status = 400;
            res.set_content(nlohmann::json{{"error", {
                {"message", "Query parameter 'query' must contain at least 3 characters"},
                {"type", "invalid_request_error"}
            }}}.dump(), "application/json");
            return;
        }

        const std::string source_text = req.has_param("source")
            ? req.get_param_value("source") : "huggingface";
        const auto source = parse_remote_registry_source(source_text);

        std::size_t limit = 12;
        if (req.has_param("limit")) {
            const std::string limit_text = req.get_param_value("limit");
            std::size_t parsed = 0;
            try {
                std::size_t consumed = 0;
                parsed = static_cast<std::size_t>(std::stoul(limit_text, &consumed));
                if (consumed != limit_text.size()) throw std::invalid_argument("trailing characters");
            } catch (...) {
                throw std::invalid_argument("Query parameter 'limit' must be an integer from 1 to 50");
            }
            if (parsed < 1 || parsed > 50) {
                throw std::invalid_argument("Query parameter 'limit' must be an integer from 1 to 50");
            }
            limit = parsed;
        }

        bool gguf_only = false;
        if (req.has_param("format")) {
            const std::string format = req.get_param_value("format");
            if (format != "gguf") {
                throw std::invalid_argument(
                    "Unsupported registry search format '" + format + "' (expected 'gguf')");
            }
            gguf_only = true;
        }

        if (config_->offline()) {
            res.status = 400;
            res.set_content(nlohmann::json{{"error", {
                {"message", "Lemond is in offline mode, registry search is unavailable"},
                {"type", "invalid_request_error"},
                {"code", "lemond_offline"}
            }}}.dump(), "application/json");
            return;
        }

        const auto search = search_registry_models(source, query, limit, gguf_only);
        nlohmann::json results = nlohmann::json::array();
        for (const auto& model : search.results) {
            results.push_back({
                {"repository_id", model.repo_id},
                {"display_name", model.display_name},
                {"source", remote_registry_source_name(model.source)},
                {"repository_type", model.repository_type},
                {"description", model.description},
                {"tags", model.tags},
                {"task", model.task},
                {"downloads", model.downloads},
                {"likes", model.likes},
                {"has_gguf", model.has_gguf}
            });
        }
        nlohmann::json response = {
            {"source", remote_registry_source_name(source)},
            {"query", query},
            {"total", search.total},
            {"results", std::move(results)}
        };
        if (gguf_only) response["format"] = "gguf";
        res.set_content(response.dump(), "application/json");
    } catch (const RegistrySearchError& e) {
        res.status = e.status_code() == 429 ? 429 : 502;
        res.set_content(nlohmann::json{{"error", {
            {"message", e.what()},
            {"type", "registry_error"},
            {"upstream_status_code", e.status_code()}
        }}}.dump(), "application/json");
    } catch (const std::invalid_argument& e) {
        res.status = 400;
        res.set_content(nlohmann::json{{"error", {
            {"message", e.what()},
            {"type", "invalid_request_error"}
        }}}.dump(), "application/json");
    } catch (const std::exception& e) {
        LOG(ERROR, "Server") << "ERROR in handle_registry_search: " << e.what() << std::endl;
        res.status = 502;
        res.set_content(nlohmann::json{{"error", {
            {"message", e.what()},
            {"type", "registry_error"}
        }}}.dump(), "application/json");
    }
}

void Server::handle_pull_variants(const httplib::Request& req, httplib::Response& res) {
    auto bad_request = [&res](const std::string& message) {
        res.status = 400;
        nlohmann::json error = {{"error", message}};
        res.set_content(error.dump(), "application/json");
    };
    try {
        std::string checkpoint = req.get_param_value("checkpoint");
        if (checkpoint.empty()) {
            res.status = 400;
            nlohmann::json error = {{"error", "Missing required query parameter 'checkpoint'"}};
            res.set_content(error.dump(), "application/json");
            return;
        }
        // Detect the URL provider upfront so we can reject mismatches with
        // an explicit `--source` param (just like /pull and the CLI do).
        lemon::RemoteRegistrySource url_source;
        std::string url_repo_id;
        bool has_url = lemon::detect_registry_url(checkpoint, url_source, url_repo_id);

        // Explicit `--source` was provided (or not).  Capture the raw param
        // now while checkpoint still holds the URL for error messages.
        std::string raw_source;
        if (req.has_param("source")) {
            raw_source = req.get_param_value("source");
        }

        // Reject mismatches between explicit source and detected URL source.
        // Canonicalize the raw param first so accepted aliases (e.g. `hf`) are
        // not rejected against the URL's canonical registry name.
        if (has_url && !raw_source.empty()) {
            if (parse_remote_registry_source(raw_source) != url_source) {
                bad_request(
                    "checkpoint URL uses " + remote_registry_source_name(url_source) +
                    " but source was set to '" + raw_source + "'");
                return;
            }
        }

        // A provider URL is authoritative: normalize it to owner/repo and adopt
        // its registry. Otherwise honor an explicit source param, falling back
        // to the server's configured default when none is given.
        if (has_url) {
            checkpoint = url_repo_id;
        }
        std::string source;
        if (has_url) {
            source = remote_registry_source_name(url_source);
        } else {
            source = req.has_param("source")
                ? req.get_param_value("source") : config_->default_model_source();
        }
        if (checkpoint.find('/') == std::string::npos) {
            res.status = 400;
            nlohmann::json error = {{"error",
                "Malformed 'checkpoint': expected a repository id of the form 'owner/name'"}};
            res.set_content(error.dump(), "application/json");
            return;
        }
        const auto parsed_source = parse_remote_registry_source(source);
        if (config_->offline()) {
            res.status = 400;
            nlohmann::json error = {{"error", "Lemond is in offline mode, models not downloaded"}, {"code", "lemond_offline"}};
            res.set_content(error.dump(), "application/json");
            return;
        }
        bool not_found = false;
        const std::string resolved_source = remote_registry_source_name(parsed_source);
        nlohmann::json body = lemon::fetch_pull_variants(
            checkpoint, resolved_source, not_found);
        if (not_found) {
            res.status = 404;
            nlohmann::json error = {{"error", "Checkpoint '" + checkpoint + "' not found on " +
                remote_registry_display_name(parsed_source)}};
            res.set_content(error.dump(), "application/json");
            return;
        }
        if (body.is_object()) {
            body["source"] = resolved_source;
        }
        res.set_content(body.dump(), "application/json");
    } catch (const std::invalid_argument& e) {
        res.status = 400;
        nlohmann::json error = {{"error", e.what()}};
        res.set_content(error.dump(), "application/json");
    } catch (const std::exception& e) {
        LOG(ERROR, "Server") << "ERROR in handle_pull_variants: " << e.what() << std::endl;
        res.status = 500;
        nlohmann::json error = {{"error", e.what()}};
        res.set_content(error.dump(), "application/json");
    }
}

void Server::handle_load(const httplib::Request& req, httplib::Response& res) {
    auto thread_id = std::this_thread::get_id();
    LOG(DEBUG, "Server") << "===== LOAD ENDPOINT ENTERED (Thread: " << thread_id << ") =====" << std::endl;

    // Declare model_name outside try block so it's available in catch block
    std::string model_name;

    nlohmann::json request_json;
    if (!parse_required_json_body(req, res, request_json)) return;
    normalize_and_resolve_request_model(request_json);

    try {
        model_name = request_json["model_name"];

        // Cloud models are registered automatically at cache build / cloud-auth
        // / install time, so a cloud model reaching /load is already in the
        // cache. /load carries no creds payload; CloudServer reads the
        // resolved key (env var or runtime POST) from CloudProviderRegistry
        // when a request is actually forwarded.

        // Get model info
        if (!model_manager_->model_exists(model_name)) {
            LOG(ERROR, "Server") << "Model not found: " << model_name << std::endl;
            res.status = 404;
            auto error_response = create_model_error(model_name, "Model not found");
            res.set_content(error_response.dump(), "application/json");
            return;
        }

        auto info = model_manager_->get_model_info(model_name);

        // Extract optional per-model settings. Omitted options keep saved values;
        // null masks only its saved key for this load. Concrete *_args scope is
        // resolved later by Router. ctx_size=-1 remains an explicit auto value.
        RecipeOptions options = RecipeOptions(info.recipe, request_json);
        std::set<std::string> transient_saved_option_tombstones;
        for (const auto& key : RecipeOptions::keys_for_recipe(info.recipe)) {
            if (request_json.contains(key) && request_json[key].is_null()) {
                transient_saved_option_tombstones.insert(key);
            }
        }

        bool save_options = request_json.value("save_options", false);
        std::optional<bool> pinned_opt = std::nullopt;
        if (request_json.contains("pinned") && request_json["pinned"].is_boolean()) {
            pinned_opt = request_json["pinned"].get<bool>();
        }

        LOG(INFO, "Server") << "Ensuring model loaded: " << model_name;
        LOG(INFO, "Server") << " " << options.to_log_string(false);
        LOG(INFO, "Server") << std::endl;

        // Persist concrete request options if requested. A null tombstone is
        // load-scoped, so preserve the existing saved value for that key.
        if (save_options) {
            json saved_for_write = options.to_json();
            if (!transient_saved_option_tombstones.empty()) {
                const json existing_saved = model_manager_->get_saved_model_options(model_name);
                for (const auto& key : transient_saved_option_tombstones) {
                    auto it = existing_saved.find(key);
                    if (it != existing_saved.end()) {
                        saved_for_write[key] = *it;
                    }
                }
            }
            model_manager_->set_saved_model_options(model_name, saved_for_write);
            info = model_manager_->get_model_info(model_name);
        }

        if (!model_manager_->is_model_downloaded(model_name) && !is_model_collection_recipe(info.recipe)) {
            LOG(INFO, "Server") << "Model not downloaded, downloading..." << std::endl;
            model_manager_->download_registered_model(info);
            info = model_manager_->get_model_info(model_name);
        }

        // Null tombstones are load-local: remove only those saved keys from the
        // local model layer without changing recipe_options.json.
        if (!transient_saved_option_tombstones.empty()) {
            json per_model_options = model_manager_->get_model_default_options(info).to_json();
            const json saved = model_manager_->get_saved_model_options(model_name);
            if (saved.is_object()) {
                for (const auto& [key, value] : saved.items()) {
                    if (transient_saved_option_tombstones.count(key) == 0) {
                        per_model_options[key] = value;
                    }
                }
            }
            info.recipe_options = RecipeOptions(info.recipe, per_model_options);
        }


        // Collection models: load each component instead
        if (is_omni_collection_recipe(info.recipe) && !info.components.empty()) {
            ensure_collection_loaded(info);

            nlohmann::json response = {
                {"status", "success"},
                {"model_name", model_name},
                {"recipe", info.recipe}
            };
            res.set_content(response.dump(), "application/json");
        } else if (is_router_collection_recipe(info.recipe)) {
            // Router collections are virtual: each request is dispatched to one
            // candidate at request time, which lazy-loads it. There is no backend
            // of the collection's own to bring up, and eagerly loading every
            // candidate would thrash the model LRU. Acknowledge without loading.
            nlohmann::json response = {
                {"status", "success"},
                {"model_name", model_name},
                {"recipe", info.recipe}
            };
            res.set_content(response.dump(), "application/json");
        } else {
            // Load model with optional per-model settings (declarative: no-op
            // if already loaded with matching options, reload only if options
            // differ)
            router_->load_model(model_name, info, options, true,
                                /*allow_reload_on_option_change=*/true,
                                pinned_opt);

            // Return success response
            nlohmann::json response = {
                {"status", "success"},
                {"model_name", model_name},
                {"checkpoint", info.checkpoint()},
                {"recipe", info.recipe}
            };
            res.set_content(response.dump(), "application/json");
        }

    } catch (const std::exception& e) {
        LOG(ERROR, "Server") << "Failed to load model: " << e.what() << std::endl;

        // Use consistent error format
        if (!model_name.empty()) {
            auto error_response = create_model_error(model_name, e.what());
            std::string error_code = error_response["error"]["code"].get<std::string>();
            res.status = get_http_status_from_error(error_code);
            res.set_content(error_response.dump(), "application/json");
        } else {
            // JSON parsing failed before we got model_name - return generic error
            res.status = 400;
            nlohmann::json error = {{"error", {
                {"message", std::string("Invalid request: ") + e.what()},
                {"type", "invalid_request_error"},
                {"code", "invalid_request"}
            }}};
            res.set_content(error.dump(), "application/json");
        }
    }
}

void Server::handle_unload(const httplib::Request& req, httplib::Response& res) {
    try {
        LOG(INFO, "Server") << "Unload request received" << std::endl;
        LOG(DEBUG, "Server") << "Request method: " << req.method << ", body length: " << req.body.length() << std::endl;
        LOG(DEBUG, "Server") << "Content-Type: " << req.get_header_value("Content-Type") << std::endl;

        // Multi-model support: Optional model_name parameter
        std::string model_name;
        if (!req.body.empty()) {
            try {
                auto request_json = nlohmann::json::parse(req.body);
                normalize_client_model_name(request_json);
                normalize_and_resolve_request_model(request_json);
                if (request_json.contains("model_name") && request_json["model_name"].is_string()) {
                    model_name = request_json["model_name"].get<std::string>();
                } else if (request_json.contains("model") && request_json["model"].is_string()) {
                    model_name = request_json["model"].get<std::string>();
                }
            } catch (...) {
                // Ignore parse errors, just unload all
            }
        }

        router_->unload_model(model_name);  // Empty string = unload all

        if (model_name.empty()) {
            LOG(INFO, "Server") << "All models unloaded successfully" << std::endl;
            nlohmann::json response = {
                {"status", "success"},
                {"message", "All models unloaded successfully"}
            };
            res.status = 200;
            res.set_content(response.dump(), "application/json");
        } else {
            LOG(INFO, "Server") << "Model '" << model_name << "' unloaded successfully" << std::endl;
            nlohmann::json response = {
                {"status", "success"},
                {"message", "Model unloaded successfully"},
                {"model_name", model_name}
            };
            res.status = 200;
            res.set_content(response.dump(), "application/json");
        }
    } catch (const std::exception& e) {
        LOG(ERROR, "Server") << "Unload failed: " << e.what() << std::endl;

        // Check if error is "Model not loaded" for 404
        std::string error_msg = e.what();
        if (error_msg.find("not loaded") != std::string::npos) {
            res.status = 404;
        } else {
            res.status = 500;
        }

        nlohmann::json error = {{"error", e.what()}};
        res.set_content(error.dump(), "application/json");
    }
}

void Server::handle_pin(const httplib::Request& req, httplib::Response& res) {
    try {
        nlohmann::json request_json;
        try {
            request_json = nlohmann::json::parse(req.body);
        } catch (const std::exception& parse_err) {
            res.status = 400;
            res.set_content(nlohmann::json{{"error", {
                {"message", "Invalid JSON body: " + std::string(parse_err.what())},
                {"type", "invalid_request_error"}
            }}}.dump(), "application/json");
            return;
        }

        if (!request_json.contains("model") && !request_json.contains("model_name")) {
            res.status = 400;
            res.set_content(nlohmann::json{{"error", {
                {"message", "Parameter 'model' or 'model_name' is required"},
                {"type", "invalid_request_error"}
            }}}.dump(), "application/json");
            return;
        }

        if (!request_json.contains("pinned") || !request_json["pinned"].is_boolean()) {
            res.status = 400;
            res.set_content(nlohmann::json{{"error", {
                {"message", "Parameter 'pinned' is required and must be a boolean"},
                {"type", "invalid_request_error"}
            }}}.dump(), "application/json");
            return;
        }

        std::string model_name = request_json.contains("model") ?
            request_json["model"].get<std::string>() :
            request_json["model_name"].get<std::string>();
        bool pinned = request_json["pinned"].get<bool>();

        std::string canonical_name = model_manager_->resolve_model_name(model_name);
        router_->set_model_pinned(canonical_name, pinned);

        nlohmann::json response = {
            {"status", "success"},
            {"model_name", model_name},
            {"pinned", pinned}
        };
        res.status = 200;
        res.set_content(response.dump(), "application/json");
    } catch (const std::exception& e) {
        LOG(ERROR, "Server") << "Pin/unpin failed: " << e.what() << std::endl;
        res.status = 400;
        nlohmann::json error = {{"error", {
            {"message", e.what()},
            {"type", "invalid_request_error"}
        }}};
        res.set_content(error.dump(), "application/json");
    }
}

void Server::handle_delete(const httplib::Request& req, httplib::Response& res) {
    nlohmann::json request_json;
    if (!parse_required_json_body(req, res, request_json)) return;

    try {
        // Accept both "model" and "model_name" for compatibility
        std::string model_name = request_json.contains("model") ?
            request_json["model"].get<std::string>() :
            request_json["model_name"].get<std::string>();

        LOG(INFO, "Server") << "Deleting model: " << model_name << std::endl;

        // If the model is currently loaded, unload it first to release file locks
        if (router_->is_model_loaded(model_name)) {
            LOG(INFO, "Server") << "Model is loaded, unloading before delete: " << model_name << std::endl;
            router_->unload_model(model_name);
        }

        // Retry delete with delays to handle in-progress downloads releasing file handles
        // This handles the race condition where a cancelled download hasn't yet released
        // its file handles when the delete request arrives
        const int max_retries = 3;
        const int retry_delay_seconds = 5;
        std::string last_error;

        for (int attempt = 0; attempt <= max_retries; ++attempt) {
            try {
                model_manager_->delete_model(model_name);

                // Success - send response and return
                nlohmann::json response = {
                    {"status", "success"},
                    {"message", "Deleted model: " + model_name}
                };
                res.set_content(response.dump(), "application/json");
                return;

            } catch (const std::exception& e) {
                last_error = e.what();

                // Only retry on "file in use" type errors (Windows and POSIX patterns)
                bool is_file_locked =
                    last_error.find("being used by another process") != std::string::npos ||
                    last_error.find("Permission denied") != std::string::npos ||
                    last_error.find("resource busy") != std::string::npos;

                if (is_file_locked && attempt < max_retries) {
                    LOG(INFO, "Server") << "Delete failed (file in use), retry "
                              << (attempt + 1) << "/" << max_retries
                              << " in " << retry_delay_seconds << "s..." << std::endl;
                    std::this_thread::sleep_for(std::chrono::seconds(retry_delay_seconds));
                    continue;
                }

                // Non-retryable error or max retries exceeded - rethrow
                throw;
            }
        }

    } catch (const std::exception& e) {
        LOG(ERROR, "Server") << "ERROR in handle_delete: " << e.what() << std::endl;

        // Check if this is a "Model not found" error (return 422)
        std::string error_msg = e.what();
        if (error_msg.find("Model not found") != std::string::npos ||
            error_msg.find("not supported") != std::string::npos) {
            res.status = 422;
        } else {
            res.status = 500;
        }

        nlohmann::json error = {{"error", e.what()}};
        res.set_content(error.dump(), "application/json");
    }
}

void Server::handle_cleanup_cache(const httplib::Request& req, httplib::Response& res) {
    try {
        auto request_json = nlohmann::json::parse(req.body);
        bool dry_run = request_json.value("dry_run", true);

        auto result = model_manager_->cleanup_orphaned_cache(dry_run);
        res.set_content(result.dump(), "application/json");
    } catch (const std::exception& e) {
        LOG(ERROR, "Server") << "ERROR in handle_cleanup_cache: " << e.what() << std::endl;
        res.status = 500;
        auto error_response = create_model_error("", e.what());
        res.set_content(error_response.dump(), "application/json");
    }
}

// Alias failures are not model-lookup failures. create_model_error() rewrites its
// message whenever the name is absent from the registry, which is always true for a
// new alias, so routing these through it would report "model not found" instead of
// the actual reason.
static nlohmann::json create_alias_error(const std::string& message, const std::string& code,
                                         const std::string& type = "invalid_request_error") {
    return nlohmann::json{{"error", {
        {"message", message},
        {"type", type},
        {"param", "alias"},
        {"code", code}
    }}};
}

void Server::handle_aliases_get(const httplib::Request& req, httplib::Response& res) {
    try {
        nlohmann::json alias_list = nlohmann::json::array();
        if (alias_manager_) {
            for (const auto& [alias, target] : alias_manager_->get_all_aliases()) {
                bool downloaded = false;
                std::string recipe = "llamacpp";
                try {
                    if (model_manager_->model_exists(target)) {
                        std::string canonical = model_manager_->resolve_model_name(target);
                        downloaded = model_manager_->is_model_downloaded(canonical);
                        ModelInfo info = model_manager_->get_model_info(canonical);
                        recipe = info.recipe;
                    }
                } catch (...) {}
                alias_list.push_back({
                    {"alias", alias},
                    {"target", target},
                    {"downloaded", downloaded},
                    {"recipe", recipe}
                });
            }
        }
        res.set_content(nlohmann::json{{"aliases", alias_list}}.dump(), "application/json");
    } catch (const std::exception& e) {
        res.status = 500;
        res.set_content(create_alias_error(e.what(), "internal_error", "server_error").dump(), "application/json");
    }
}

void Server::handle_aliases_add(const httplib::Request& req, httplib::Response& res) {
    try {
        auto req_json = nlohmann::json::parse(req.body);
        std::string alias = utils::normalize_model_name(req_json.value("alias", ""));
        std::string target = utils::normalize_model_name(req_json.value("target", req_json.value("model", "")));

        if (alias.empty() || target.empty()) {
            res.status = 400;
            res.set_content(create_alias_error("Alias and target fields are required", "invalid_request").dump(), "application/json");
            return;
        }

        if (alias == target) {
            res.status = 400;
            res.set_content(create_alias_error("Alias cannot point to itself", "invalid_alias").dump(), "application/json");
            return;
        }

        if (alias.rfind("user.", 0) == 0 || alias.rfind("extra.", 0) == 0 || alias.rfind("builtin.", 0) == 0) {
            res.status = 400;
            res.set_content(create_alias_error("Alias name cannot use reserved prefixes (user., extra., builtin.)", "invalid_alias").dump(), "application/json");
            return;
        }

        if (model_manager_->model_exists(alias)) {
            std::string canonical = model_manager_->resolve_model_name(alias);
            if (canonical == alias || canonical == "user." + alias || canonical == "builtin." + alias) {
                res.status = 409;
                res.set_content(create_alias_error("Cannot create alias '" + alias + "': Name conflicts with an existing canonical model", "alias_conflict").dump(), "application/json");
                return;
            }
        }

        if (!alias_manager_) {
            res.status = 500;
            res.set_content(create_alias_error("AliasManager uninitialized", "internal_error", "server_error").dump(), "application/json");
            return;
        }

        std::string err_msg;
        if (!alias_manager_->set_alias(alias, target, err_msg)) {
            res.status = 409;
            res.set_content(create_alias_error(err_msg, "alias_conflict").dump(), "application/json");
            return;
        }

        nlohmann::json response = {
            {"status", "ok"},
            {"alias", alias},
            {"target", target}
        };
        res.set_content(response.dump(), "application/json");
    } catch (const std::exception& e) {
        res.status = 400;
        res.set_content(create_alias_error(e.what(), "invalid_request").dump(), "application/json");
    }
}

void Server::handle_aliases_remove(const httplib::Request& req, httplib::Response& res) {
    std::string alias = utils::normalize_model_name(req.matches[1]);
    try {
        if (!alias_manager_ || !alias_manager_->remove_alias(alias)) {
            res.status = 404;
            res.set_content(create_alias_error("Alias not found: " + alias, "alias_not_found").dump(), "application/json");
            return;
        }
        nlohmann::json response = {
            {"status", "deleted"},
            {"alias", alias}
        };
        res.set_content(response.dump(), "application/json");
    } catch (const std::exception& e) {
        res.status = 500;
        res.set_content(create_alias_error(e.what(), "internal_error", "server_error").dump(), "application/json");
    }
}

std::string Server::resolve_alias_target(const std::string& model_name) const {
    if (alias_manager_) {
        auto resolved = alias_manager_->resolve_alias(model_name);
        if (resolved.has_value()) {
            return resolved.value();
        }
    }
    return model_name;
}

void Server::normalize_and_resolve_request_model(nlohmann::json& request_json) const {
    if (request_json.contains("model") && request_json["model"].is_string()) {
        std::string raw_name = request_json["model"].get<std::string>();
        std::string resolved = resolve_alias_target(raw_name);
        if (resolved != raw_name) {
            request_json["model"] = resolved;
        }
    }
    if (request_json.contains("model_name") && request_json["model_name"].is_string()) {
        std::string raw_name = request_json["model_name"].get<std::string>();
        std::string resolved = resolve_alias_target(raw_name);
        if (resolved != raw_name) {
            request_json["model_name"] = resolved;
        }
    }
}

void Server::persist_cloud_providers() {
    if (config_dir_.empty() || !cloud_registry_) return;
    try {
        json user_cfg = ConfigFile::load_raw(config_dir_);
        user_cfg["cloud_providers"] = cloud_registry_->to_config_array();
        ConfigFile::save(config_dir_, user_cfg);
    } catch (const std::exception& e) {
        // Persistence failure must not undo the in-memory change — the
        // registry is already updated and the provider is usable until
        // restart. Log so the operator can see why config.json is stale.
        LOG(WARNING, "Server") << "Failed to persist cloud_providers to config.json: "
                                << e.what() << std::endl;
    }
}

void Server::handle_cloud_auth_set(const httplib::Request& req, httplib::Response& res) {
    try {
        const auto body = nlohmann::json::parse(req.body);
        if (!body.contains("provider") || !body["provider"].is_string() ||
            !body.contains("api_key") || !body["api_key"].is_string()) {
            res.status = 400;
            nlohmann::json error = {{"error", {
                {"message", "Body must contain string fields: provider, api_key"},
                {"type", "invalid_request_error"}}}};
            res.set_content(error.dump(), "application/json");
            return;
        }
        const auto provider = body["provider"].get<std::string>();
        const auto api_key = body["api_key"].get<std::string>();
        bool allow_insecure_http = false;
        if (body.contains("allow_insecure_http")) {
            if (!body["allow_insecure_http"].is_boolean()) {
                res.status = 400;
                nlohmann::json error = {{"error", {
                    {"message", "allow_insecure_http must be a boolean when provided"},
                    {"type", "invalid_request_error"}}}};
                res.set_content(error.dump(), "application/json");
                return;
            }
            allow_insecure_http = body["allow_insecure_http"].get<bool>();
        }
        if (provider.empty() || api_key.empty()) {
            res.status = 400;
            nlohmann::json error = {{"error", {
                {"message", "provider and api_key must be non-empty"},
                {"type", "invalid_request_error"}}}};
            res.set_content(error.dump(), "application/json");
            return;
        }

        if (!cloud_registry_->is_installed(provider)) {
            res.status = 404;
            nlohmann::json error = {{"error", {
                {"message", "Cloud provider '" + provider + "' is not installed. "
                            "Call POST /v1/install with backend=cloud, provider, "
                            "and base_url first."},
                {"type", "invalid_request_error"}}}};
            res.set_content(error.dump(), "application/json");
            return;
        }

        const std::string base_url = cloud_registry_->base_url_for(provider);
        if (CloudProviderRegistry::is_http_base_url(base_url)) {
            const bool already_allowed = cloud_registry_->allow_insecure_http_for(provider);
            if (!allow_insecure_http && !already_allowed) {
                res.status = 400;
                nlohmann::json error = {{"error", {
                    {"message", "Cloud provider '" + provider + "' uses http://. "
                                "Set allow_insecure_http=true to explicitly opt in before "
                                "storing or using an API key over plaintext HTTP."},
                    {"type", "invalid_request_error"},
                    {"code", "insecure_http_requires_opt_in"}}}};
                res.set_content(error.dump(), "application/json");
                return;
            }
            if (allow_insecure_http && !already_allowed) {
                cloud_registry_->set_allow_insecure_http(provider, true);
                persist_cloud_providers();
            }
        }

        // env-wins-over-runtime: if the env var is set, refuse the runtime
        // key with 409 so the caller knows their POST had no effect.
        if (!cloud_registry_->set_runtime_key(provider, api_key)) {
            res.status = 409;
            const auto env_name = CloudProviderRegistry::env_var_name(provider);
            nlohmann::json error = {{"error", {
                {"message", env_name + " is set in the lemond process; the env var "
                            "takes precedence and the supplied API key was not stored."},
                {"type", "auth_conflict"},
                {"env_var", env_name}}}};
            res.set_content(error.dump(), "application/json");
            return;
        }

        // Refresh the provider's discovered model list with the new key.
        // Best-effort — refresh logs failures and returns 0. Either way the
        // key is stored and the response reflects the auth state.
        size_t models_after = model_manager_->refresh_cloud_models(provider);

        const auto state = cloud_registry_->auth_state(provider);
        nlohmann::json response = {
            {"provider", provider},
            {"allow_insecure_http", cloud_registry_->allow_insecure_http_for(provider)},
            {"auth_state", {
                {"env_var_set", state.env_var_set},
                {"runtime_key_set", state.runtime_key_set}
            }},
            {"models_discovered", models_after}
        };
        attach_warnings(
            response,
            CloudProviderRegistry::base_url_warnings(
                base_url,
                /*api_key_available=*/true));
        res.set_content(response.dump(), "application/json");
    } catch (const nlohmann::json::parse_error& e) {
        res.status = 400;
        nlohmann::json error = {{"error", {{"message", "Invalid JSON: " + std::string(e.what())},
                                            {"type", "invalid_request_error"}}}};
        res.set_content(error.dump(), "application/json");
    } catch (const std::exception& e) {
        LOG(ERROR, "Server") << "ERROR in handle_cloud_auth_set: " << e.what() << std::endl;
        res.status = 500;
        nlohmann::json error = {{"error", {{"message", e.what()}, {"type", "server_error"}}}};
        res.set_content(error.dump(), "application/json");
    }
}

void Server::handle_cloud_auth_clear(const httplib::Request& req, httplib::Response& res) {
    try {
        const std::string provider = req.matches[1].str();
        if (provider.empty()) {
            res.status = 400;
            nlohmann::json error = {{"error", {{"message", "Missing provider in URL"},
                                                {"type", "invalid_request_error"}}}};
            res.set_content(error.dump(), "application/json");
            return;
        }

        const bool cleared = cloud_registry_->clear_runtime_key(provider);
        // If the env var is set, models stay discovered (env still authenticates);
        // if not, the cache should reflect the now-unauthenticated state.
        const auto state = cloud_registry_->auth_state(provider);
        if (!state.env_var_set) {
            model_manager_->evict_cloud_models(provider);
        }

        nlohmann::json response = {
            {"provider", provider},
            {"cleared_runtime_key", cleared},
            {"auth_state", {
                {"env_var_set", state.env_var_set},
                {"runtime_key_set", state.runtime_key_set}
            }}
        };
        res.set_content(response.dump(), "application/json");
    } catch (const std::exception& e) {
        LOG(ERROR, "Server") << "ERROR in handle_cloud_auth_clear: " << e.what() << std::endl;
        res.status = 500;
        nlohmann::json error = {{"error", {{"message", e.what()}, {"type", "server_error"}}}};
        res.set_content(error.dump(), "application/json");
    }
}

// Called by handle_pull when local_import=true
// Parameters:
//   - dest_path: Directory where model files are located (already copied/uploaded)
//   - model_name: Model name with "user." prefix
//   - model_data: Request content
//   - hf_cache: HuggingFace cache directory for computing relative paths
void Server::resolve_and_register_local_model(
    const std::string& dest_path,
    const std::string& model_name,
    const json& model_data,
    const std::string& hf_cache) {
    std::string mmproj = model_data.value("mmproj", "");
    std::string recipe = model_data.value("recipe", "");
    bool vision = model_data.value("vision", false);

    // The backend's ops locate its primary artifact within the imported
    // directory (.gguf / .bin file, genai_config.json dir, …); "" means register
    // the directory itself.
    std::string resolved_checkpoint = backends::ops_for(recipe)->find_imported_checkpoint(dest_path);
    std::string resolved_mmproj;

    // Search for mmproj file if vision is enabled or mmproj hint provided
    if (vision || !mmproj.empty()) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(dest_path)) {
            if (entry.is_regular_file()) {
                std::string filename = entry.path().filename().string();
                std::string filename_lower = filename;
                std::transform(filename_lower.begin(), filename_lower.end(), filename_lower.begin(), ::tolower);

                // Match either the provided mmproj name or any mmproj file
                if (!mmproj.empty() && filename == mmproj) {
                    resolved_mmproj = filename;
                    vision = true;  // Ensure vision is set
                    break;
                } else if (filename_lower.find("mmproj") != std::string::npos) {
                    resolved_mmproj = filename;
                    vision = true;  // Ensure vision is set
                    break;
                }
            }
        }
    }

    // Build checkpoint for registration - store as relative path from HF cache
    std::string checkpoint_to_register;
    std::filesystem::path hf_cache_path = utils::path_from_utf8(hf_cache);
    if (!resolved_checkpoint.empty()) {
        std::filesystem::path rel = std::filesystem::relative(
            utils::path_from_utf8(resolved_checkpoint), hf_cache_path);
        checkpoint_to_register = utils::path_to_utf8(rel);
    } else {
        // Fallback - use dest_path relative to hf_cache
        std::filesystem::path rel = std::filesystem::relative(
            utils::path_from_utf8(dest_path), hf_cache_path);
        checkpoint_to_register = utils::path_to_utf8(rel);
    }

    LOG(INFO, "Server") << "Registering model with checkpoint: " << checkpoint_to_register << std::endl;

    auto actual_model_data = model_data;
    actual_model_data["checkpoint"] = checkpoint_to_register;
    if (!resolved_mmproj.empty()) {
        actual_model_data["mmproj"] = resolved_mmproj;
    }

    // Register the model with source to mark how it was added
    model_manager_->register_user_model(
        model_name,
        actual_model_data,
        "local_upload"
    );

    LOG(INFO, "Server") << "Model registered successfully" << std::endl;
}

void Server::handle_stats(const httplib::Request& req, httplib::Response& res) {
    // For HEAD requests, just return 200 OK without processing
    if (req.method == "HEAD") {
        res.status = 200;
        return;
    }

    try {
        auto stats = router_->get_stats();
        res.set_content(stats.dump(), "application/json");
    } catch (const std::exception& e) {
        LOG(ERROR, "Server") << "ERROR in handle_stats: " << e.what() << std::endl;
        res.status = 500;
        nlohmann::json error = {{"error", e.what()}};
        res.set_content(error.dump(), "application/json");
    }
}

void Server::handle_metrics(const httplib::Request& req, httplib::Response& res) {
    if (req.method == "HEAD") {
        res.status = 200;
        return;
    }

    try {
        SystemMetrics system_metrics;
        system_metrics.cpu_percent = get_cpu_usage();
        const auto gpu_metrics = metrics_platform_->get_system_gpu_metrics();
        system_metrics.gpu_percent = gpu_metrics.gpu_percent;
        system_metrics.vram_gb = gpu_metrics.vram_used_gb;
        system_metrics.npu_percent = get_npu_utilization();

        res.set_content(build_prometheus_metrics(*router_, system_metrics),
                        "text/plain; version=0.0.4; charset=utf-8");
    } catch (const std::exception& e) {
        LOG(ERROR, "Server") << "ERROR in handle_metrics: " << e.what() << std::endl;
        res.status = 500;
        res.set_content("# Lemonade metrics error\n", "text/plain; version=0.0.4; charset=utf-8");
    }
}

void Server::handle_system_info(const httplib::Request& req, httplib::Response& res) {
    // For HEAD requests, just return 200 OK without processing
    if (req.method == "HEAD") {
        res.status = 200;
        return;
    }

    // SystemInfoCache is the single source of truth for hardware + recipes.
    // Recipes are cached until invalidated by install/uninstall.
    nlohmann::json system_info = SystemInfoCache::get_system_info_with_cache();

    // Enrich with release_url, download_filename, version from BackendManager config
    if (system_info.contains("recipes")) {
        enrich_recipes(system_info["recipes"]);
    }

    // Surfaced as a separate host-specific field rather than by pruning
    // `recipes`, which must stay canonical: the docs generator renders
    // README/models.js from it and would otherwise drift with the generating
    // machine's memory. Dynamic-model backends (cloud, flm) are never listed.
    if (model_manager_) {
        nlohmann::json unavailable = nlohmann::json::array();
        for (const std::string& recipe : model_manager_->recipes_with_all_models_filtered()) {
            const BackendDescriptor* desc = backends::descriptor_for(recipe);
            if (desc && desc->dynamic_models) {
                continue;
            }
            unavailable.push_back(recipe);
        }
        system_info["unavailable_recipes"] = std::move(unavailable);
    }

    // Surface runtime config flags that affect client-side install/download UX.
    if (auto* cfg = RuntimeConfig::global()) {
        system_info["no_fetch_executables"] = cfg->no_fetch_executables();
    }

    if (config_) {
        system_info["model_storage"] = get_model_storage_stats(utils::get_hf_cache_dir());
    }

    // Cloud providers: per-provider {name, base_url, env_var_set,
    // runtime_key_set, models_discovered}. Never includes the key itself.
    // Clients use this to decide whether to prompt for an API key, show a
    // "configured by env var" badge, or surface the model count to the user.
    if (cloud_registry_) {
        nlohmann::json providers = nlohmann::json::array();
        for (const auto& rec : cloud_registry_->list_installed()) {
            auto state = cloud_registry_->auth_state(rec.name);
            nlohmann::json provider = {
                {"name", rec.name},
                {"base_url", rec.base_url},
                {"allow_insecure_http", rec.allow_insecure_http},
                {"auth_header_name", rec.auth_header_name},
                {"auth_header_prefix", rec.auth_header_prefix},
                {"wire_format", rec.wire_format},
                {"env_var", CloudProviderRegistry::env_var_name(rec.name)},
                {"env_var_set", state.env_var_set},
                {"runtime_key_set", state.runtime_key_set},
                {"models_discovered", model_manager_->count_cloud_models(rec.name)}
            };
            attach_warnings(
                provider,
                CloudProviderRegistry::base_url_warnings(
                    rec.base_url,
                    state.env_var_set || state.runtime_key_set));
            providers.push_back(std::move(provider));
        }
        system_info["cloud"] = {{"providers", providers}};
    }

    res.set_content(system_info.dump(), "application/json");
}

// Get CPU usage percentage
double Server::get_cpu_usage() {
#if defined(__linux__) || defined(_WIN32)
    return metrics_platform_->get_cpu_usage(cpu_stats_mutex_,
                                            last_cpu_stats_.total,
                                            last_cpu_stats_.total_idle);
#else
    uint64_t dummy_total = 0, dummy_idle = 0;
    std::mutex dummy_mutex;
    return metrics_platform_->get_cpu_usage(dummy_mutex, dummy_total, dummy_idle);
#endif
}

// Helper: Get NPU utilization (AMD NPU on Linux)
double Server::get_npu_utilization() {
    return metrics_platform_->get_npu_utilization();
}

void Server::handle_system_stats(const httplib::Request& req, httplib::Response& res) {
    // For HEAD requests, just return 200 OK without processing
    if (req.method == "HEAD") {
        res.status = 200;
        return;
    }

    nlohmann::json stats;

    // CPU usage
    double cpu_percent = get_cpu_usage();
    stats["cpu_percent"] = (cpu_percent >= 0) ? nlohmann::json(cpu_percent) : nlohmann::json();

    // Get memory info
    stats["memory_gb"] = metrics_platform_->get_memory_usage_gb();

    const auto gpu_metrics = metrics_platform_->get_system_gpu_metrics();
    stats["gpu_percent"] = gpu_metrics.gpu_percent >= 0
        ? nlohmann::json(gpu_metrics.gpu_percent)
        : nlohmann::json();
    stats["vram_gb"] = gpu_metrics.vram_used_gb >= 0
        ? nlohmann::json(gpu_metrics.vram_used_gb)
        : nlohmann::json();
    const auto gpu_details = gpu_device_details_json(gpu_metrics);
    stats["primary_gpu_id"] = gpu_details["primary_gpu_id"];
    stats["gpus"] = gpu_details["gpus"];

    // NPU Utilization
    double npu_percent = get_npu_utilization();
    stats["npu_percent"] = (npu_percent >= 0) ? nlohmann::json(npu_percent) : nlohmann::json();

    res.set_content(stats.dump(), "application/json");
}

void Server::handle_simulate_vram_pressure(const httplib::Request& req, httplib::Response& res) {
    try {
        auto req_json = json::parse(req.body);
        double pct = req_json.value("pct", 0.0);
        router_->simulate_vram_pressure(pct);
        res.set_content(R"({"status": "ok"})", "application/json");
    } catch (const std::exception& e) {
        res.status = 400;
        res.set_content(json{{"error", e.what()}}.dump(), "application/json");
    }
}

namespace {

std::string job_id_from_path(const httplib::Request& req) {
    return req.matches.size() > 1 ? std::string(req.matches[1]) : std::string();
}

void job_error(httplib::Response& res, int status, const std::string& message) {
    res.status = status;
    res.set_content(lemon::jobs::json{{"error", message}}.dump(), "application/json");
}

}

void Server::handle_jobs_create(const httplib::Request& req, httplib::Response& res) {
    try {
        auto body = lemon::jobs::json::parse(req.body);
        const std::string name = body.value("name", "");
        lemon::jobs::json steps_json = lemon::jobs::json::array();
        if (body.contains("definition") && body["definition"].is_object()
            && body["definition"].contains("steps")) {
            steps_json = body["definition"]["steps"];
        } else if (body.contains("steps")) {
            steps_json = body["steps"];
        }
        std::vector<lemon::jobs::StepRecord> steps;
        if (steps_json.is_array())
            for (const auto& s : steps_json)
                steps.push_back(lemon::jobs::StepRecord::from_json(s));
        lemon::jobs::json inputs =
            body.contains("inputs") ? body["inputs"] : lemon::jobs::json::object();
        const std::string id = job_manager_->create(name, std::move(steps), inputs);
        res.status = 202;
        res.set_content(lemon::jobs::json{{"id", id}}.dump(), "application/json");
    } catch (const lemon::jobs::JobError& e) {
        job_error(res, e.status, e.what());
    } catch (const std::exception& e) {
        job_error(res, 400, e.what());
    }
}

void Server::handle_jobs_list(const httplib::Request&, httplib::Response& res) {
    res.set_content(lemon::jobs::json{{"jobs", job_manager_->list()}}.dump(), "application/json");
}

void Server::handle_jobs_get(const httplib::Request& req, httplib::Response& res) {
    const auto job = job_manager_->get(job_id_from_path(req));
    if (!job) {
        job_error(res, 404, "unknown job");
        return;
    }
    res.set_content(job->dump(), "application/json");
}

void Server::handle_jobs_pause(const httplib::Request& req, httplib::Response& res) {
    if (!job_manager_->pause(job_id_from_path(req))) {
        job_error(res, 404, "job not found or not pausable");
        return;
    }
    res.set_content(R"({"status":"pausing"})", "application/json");
}

void Server::handle_jobs_interrupt(const httplib::Request& req, httplib::Response& res) {
    if (!job_manager_->interrupt(job_id_from_path(req))) {
        job_error(res, 404, "job not found or not interruptible");
        return;
    }
    res.set_content(R"({"status":"interrupting"})", "application/json");
}

void Server::handle_jobs_resume(const httplib::Request& req, httplib::Response& res) {
    if (!job_manager_->resume(job_id_from_path(req))) {
        job_error(res, 404, "job not found or not resumable");
        return;
    }
    res.set_content(R"({"status":"resuming"})", "application/json");
}

void Server::handle_jobs_delete(const httplib::Request& req, httplib::Response& res) {
    bool active = false;
    if (!job_manager_->remove(job_id_from_path(req), active)) {
        job_error(res, active ? 409 : 404, active ? "job is active" : "unknown job");
        return;
    }
    res.set_content(R"({"status":"deleted"})", "application/json");
}

void Server::handle_shutdown(const httplib::Request& req, httplib::Response& res) {
    LOG(INFO, "Server") << "Shutdown request received" << std::endl;

    // Unload all models SYNCHRONOUSLY before sending the response.
    // This ensures child processes (llama-server, etc.) are terminated
    // before the caller proceeds, avoiding zombie processes.
    if (router_) {
        LOG(INFO, "Server") << "Unloading models and stopping backend servers..." << std::endl;
        try {
            router_->unload_model();
            LOG(INFO, "Server") << "All models unloaded" << std::endl;
        } catch (const std::exception& e) {
            LOG(ERROR, "Server") << "Error during unload: " << e.what() << std::endl;
        }
    }

    nlohmann::json response = {{"status", "shutting down"}};
    res.set_content(response.dump(), "application/json");

    // Stop the HTTP listener and exit asynchronously (allows response to be sent first)
    std::thread([this]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        cancel_download_jobs();
        stop();
        std::exit(0);
    }).detach();
}

void Server::handle_config_set(const httplib::Request& req, httplib::Response& res) {
    try {
        auto body = nlohmann::json::parse(req.body);

        auto result = config_->set(body, [this](const json& applied) {
            apply_config_side_effects(applied);
        });

        if (!config_dir_.empty()) {
            try {
                json user_cfg = ConfigFile::load_raw(config_dir_);
                if (result.contains("updated") && result["updated"].is_object()) {
                    user_cfg = utils::JsonUtils::merge(user_cfg, result["updated"]);
                }
                json defaults = ConfigFile::get_defaults();
                utils::JsonUtils::prune_matching(user_cfg, defaults);
                ConfigFile::save(config_dir_, user_cfg);
            } catch (const std::exception& e) {
                LOG(WARNING, "Server") << "Failed to persist config.json: " << e.what() << std::endl;
            }
        }

        res.set_content(result.dump(), "application/json");
    } catch (const nlohmann::json::parse_error& e) {
        res.status = 400;
        nlohmann::json error = {{"error", "Invalid JSON in request body"}};
        res.set_content(error.dump(), "application/json");
    } catch (const std::invalid_argument& e) {
        res.status = 400;
        nlohmann::json error = {{"error", e.what()}};
        res.set_content(error.dump(), "application/json");
    } catch (const std::exception& e) {
        LOG(ERROR, "Server") << "ERROR in handle_config_set: " << e.what() << std::endl;
        res.status = 500;
        nlohmann::json error = {{"error", e.what()}};
        res.set_content(error.dump(), "application/json");
    }
}

void Server::handle_config_get(const httplib::Request& /*req*/, httplib::Response& res) {
    try {
        auto snap = config_->snapshot();
        res.set_content(snap.dump(), "application/json");
    } catch (const std::exception& e) {
        LOG(ERROR, "Server") << "ERROR in handle_config_get: " << e.what() << std::endl;
        res.status = 500;
        nlohmann::json error = {{"error", e.what()}};
        res.set_content(error.dump(), "application/json");
    }
}

void Server::handle_config_defaults_get(const httplib::Request& /*req*/, httplib::Response& res) {
    try {
        // The canonical default config (global keys + descriptor-derived per-recipe
        // sections), independent of this host's config.json or deployment overrides.
        res.set_content(ConfigFile::base_defaults().dump(2), "application/json");
    } catch (const std::exception& e) {
        LOG(ERROR, "Server") << "ERROR in handle_config_defaults_get: " << e.what() << std::endl;
        res.status = 500;
        nlohmann::json error = {{"error", e.what()}};
        res.set_content(error.dump(), "application/json");
    }
}

void Server::handle_bin_change(const std::string& section,
                                const std::string& bin_key,
                                const std::string& new_value) {
    std::string recipe = RuntimeConfig::config_section_to_recipe(section);

    // bin_key is "<backend>_bin" — strip the suffix to get the backend name
    // expected by install_backend / find_external_backend_binary.
    std::string backend = bin_key.substr(0, bin_key.size() - 4);

    // The "server_bin" key (as in ryzenai.server_bin) is not consumed by the
    // current install flow, so skip the hot-swap rather than attempt an install
    // that won't help.
    if (backend == "server") {
        LOG(WARNING, "Server") << section << "." << bin_key
                               << " is not consumed by the install flow; "
                                  "no hot-swap performed." << std::endl;
        return;
    }

    LOG(INFO, "Server") << "*_bin config changed: " << section << "." << bin_key
                        << " = '" << new_value << "' — hot-swapping "
                        << recipe << ":" << backend << std::endl;

    // Snapshot loaded models on this (recipe, backend). A model whose options
    // do not pin a backend (mb empty) is treated as potentially affected since
    // it could resolve to the changed backend on next load.
    struct Saved {
        std::string name;
        RecipeOptions opts;
    };
    std::vector<Saved> previously_loaded;
    auto loaded = router_->get_all_loaded_models();
    std::string backend_option_key = recipe + "_backend";
    for (const auto& m : loaded) {
        if (m.value("recipe", "") != recipe) continue;
        std::string mb;
        if (m.contains("recipe_options") && m["recipe_options"].contains(backend_option_key)) {
            mb = m["recipe_options"].value(backend_option_key, "");
        }
        if (!mb.empty() && mb != backend) continue;
        std::string name = m.value("model_name", "");
        if (name.empty()) continue;
        previously_loaded.push_back({name, router_->get_model_recipe_options(name)});
    }

    for (const auto& s : previously_loaded) {
        LOG(INFO, "Server") << "Unloading " << s.name
                            << " before installing new " << recipe << ":" << backend
                            << " binary" << std::endl;
        try {
            router_->unload_model(s.name);
        } catch (const std::exception& e) {
            LOG(WARNING, "Server") << "Failed to unload " << s.name << ": " << e.what() << std::endl;
        }
    }

    // Install the new binary. install_from_github bails early when the user's
    // value resolves to a path (find_external_backend_binary returns it). When
    // version.txt mismatches the resolved version, the install dir is wiped
    // and re-downloaded.
    try {
        backend_manager_->install_backend(recipe, backend);
    } catch (const std::exception& e) {
        LOG(WARNING, "Server") << "install_backend(" << recipe << ":" << backend
                               << ") failed after *_bin change: " << e.what() << std::endl;
    }

    // Best-effort reload of previously-loaded models on the new binary.
    for (const auto& s : previously_loaded) {
        try {
            auto info = model_manager_->get_model_info(s.name);
            router_->load_model(s.name, info, s.opts, true);
            LOG(INFO, "Server") << "Reloaded " << s.name << " on new "
                                << recipe << ":" << backend << " binary" << std::endl;
        } catch (const std::exception& e) {
            LOG(WARNING, "Server") << "Failed to reload " << s.name
                                   << " after *_bin hot-swap: " << e.what() << std::endl;
        }
    }

    SystemInfoCache::invalidate_recipes();
    model_manager_->invalidate_models_cache();
}

void Server::apply_config_side_effects(const json& applied_changes) {
    for (auto& [key, value] : applied_changes.items()) {
        if (key == "port") {
            int new_port = config_->port();
            int current_port = port_.load();
            if (new_port != current_port) {
                LOG(INFO, "Server") << "Port change requested: " << current_port << " -> " << new_port << std::endl;
                port_.store(new_port);
                if (running_) {
                    rebind_requested_ = true;
                    udp_beacon_.stopBroadcasting();
                    stop_http_listeners();
                }
            }
        } else if (key == "host") {
            LOG(INFO, "Server") << "Host change requested to: " << config_->host() << std::endl;
            if (running_) {
                rebind_requested_ = true;
                udp_beacon_.stopBroadcasting();
                stop_http_listeners();
            }
            // Restart websocket server with new host
            if (websocket_server_ && running_) {
                websocket_server_->stop();
                websocket_server_ = std::make_unique<WebSocketServer>(
                    router_.get(),
                    config_->host(),
                    config_->websocket_port());
                websocket_server_->start();
            }
        } else if (key == "websocket_port") {
            if (websocket_server_) {
                LOG(INFO, "Server") << "Restarting WebSocket server on requested port "
                                    << config_->websocket_port() << std::endl;
                websocket_server_->stop();
                websocket_server_ = std::make_unique<WebSocketServer>(
                    router_.get(),
                    config_->host(),
                    config_->websocket_port());
                if (running_) {
                    websocket_server_->start();
                }
            }
        } else if (key == "log_level" || key == "log_file" || key == "log_max_file_size_mb" || key == "log_max_files") {
            LogRotationConfig rot_cfg;
            rot_cfg.file_mode = config_->log_file();
            rot_cfg.max_file_size_mb = config_->log_max_file_size_mb();
            rot_cfg.max_files = config_->log_max_files();
            LOG(INFO, "Server") << "Logging configuration updated (level=" << config_->log_level()
                                << ", file=" << rot_cfg.file_mode
                                << ", max_size=" << rot_cfg.max_file_size_mb << "MB"
                                << ", max_files=" << rot_cfg.max_files << ")" << std::endl;
            reconfigure_application_logging(config_->log_level(), rot_cfg);
        } else if (key == "global_timeout") {
            long timeout = config_->global_timeout();
            LOG(INFO, "Server") << "Global timeout changed to: " << timeout << "s" << std::endl;
            utils::HttpClient::set_default_timeout(timeout);
        } else if (key == "download_rate_limit") {
            const int64_t bps = config_->download_rate_limit_bytes_per_second();
            if (bps > 0) {
                LOG(INFO, "Server") << "Download rate limit enabled at " << bps << " B/s" << std::endl;
            } else {
                LOG(INFO, "Server") << "Download rate limit disabled" << std::endl;
            }
            utils::HttpClient::set_download_rate_limit(bps);
        } else if (key == "allowed_origins") {
            LOG(INFO, "Server") << "Allowed origins updated: " << config_->allowed_origins() << std::endl;
        } else if (key == "broadcast" || key == "no_broadcast") {
            bool bcast = config_->broadcast();
            LOG(INFO, "Server") << "Broadcast " << (bcast ? "enabled" : "disabled") << std::endl;
            if (!bcast) {
                udp_beacon_.stopBroadcasting();
            } else {
                auto rfc1918Interfaces = udp_beacon_.getLocalRFC1918Interfaces();
                if (!rfc1918Interfaces.empty()) {
                    udp_beacon_.startBroadcasting(13305, port_, 2);
                }
            }
        } else if (key == "extra_models_dir") {
            std::string dir = config_->extra_models_dir();
            LOG(INFO, "Server") << "Extra models dir changed to: " << dir << std::endl;
            model_manager_->set_extra_models_dir(dir);
        } else if (key == "models_dir") {
            std::string dir = config_->models_dir();
            LOG(INFO, "Server") << "Models dir changed to: " << dir << std::endl;
            utils::set_models_dir(dir);
            model_manager_->invalidate_models_cache();
        } else if (key == "telemetry") {
            if (value.is_object()) {
                if (value.contains("enabled")) {
                    bool enabled = config_->telemetry_enabled();
                    LOG(INFO, "Server") << "Telemetry " << (enabled ? "enabled" : "disabled") << std::endl;
                }
                if (value.contains("otlp") && value["otlp"].is_object() && value["otlp"].contains("endpoint")) {
                    LOG(INFO, "Server") << "Telemetry endpoint changed to: " << config_->telemetry_otlp_endpoint() << std::endl;
                }
            }
        } else if (value.is_object()) {
            // Nested backend section change (llamacpp / whispercpp / sdcpp / ryzenai / kokoro).
            // Recipe defaults (e.g. default_backend) are derived from these settings, so
            // drop the memoized recipes so the next /system-info recomputes them.
            SystemInfoCache::invalidate_recipes();
            // Look for *_bin sub-keys and trigger a hot-swap of the affected backend.
            for (auto& [sub_key, sub_value] : value.items()) {
                if (sub_key.size() >= 4
                    && sub_key.compare(sub_key.size() - 4, 4, "_bin") == 0) {
                    handle_bin_change(key, sub_key,
                                      sub_value.is_string() ? sub_value.get<std::string>() : "");
                }
            }
        }
    }
}


// ============================================================================
// Server-owned Download Manager state
// ============================================================================

namespace {

constexpr auto DOWNLOAD_TERMINAL_VISIBILITY = std::chrono::seconds(30);

} // namespace

nlohmann::json Server::download_progress_to_json(const DownloadProgress& p) {
    nlohmann::json event_data;
    event_data["file"] = p.file;
    event_data["file_index"] = p.file_index;
    event_data["total_files"] = p.total_files;
    event_data["bytes_downloaded"] = static_cast<uint64_t>(p.bytes_downloaded);
    event_data["bytes_total"] = static_cast<uint64_t>(p.bytes_total);
    event_data["total_download_size"] = static_cast<uint64_t>(p.total_download_size);
    event_data["bytes_previously_downloaded"] = static_cast<uint64_t>(p.bytes_previously_downloaded);
    event_data["percent"] = p.percent;
    event_data["complete"] = p.complete;
    if (!p.error.empty()) {
        event_data["error"] = p.error;
    }
    return event_data;
}

nlohmann::json Server::download_job_to_json(const std::shared_ptr<DownloadJob>& job) {
    nlohmann::json item = job->progress.is_object() ? job->progress : nlohmann::json::object();
    item["id"] = job->id;
    item["type"] = job->type;
    item["model_name"] = job->display_name;
    item["status"] = job->status;
    item["running"] = job->running;
    if (!job->error.empty()) {
        item["error"] = job->error;
    }
    return item;
}

bool Server::is_download_job_visible(const std::shared_ptr<DownloadJob>& job) const {
    if (!job) return false;
    if (job->running) {
        return true;
    }
    if (job->status == "downloading" || job->status == "paused" || job->status == "error") {
        return true;
    }
    if (job->status == "cancelled") {
        // Cancelled downloads may still have partial files on disk. Keep them
        // discoverable across reloads/restarts until the UI explicitly removes
        // the row after cleanup, retry, or user dismissal.
        return true;
    }
    if (job->status == "completed") {
        return job->terminal_since.time_since_epoch().count() > 0 &&
               std::chrono::steady_clock::now() - job->terminal_since < DOWNLOAD_TERMINAL_VISIBILITY;
    }
    return false;
}

void Server::join_download_job(const std::shared_ptr<DownloadJob>& job) {
    // Download workers capture `this`, so they must never outlive Server. Move
    // the thread out under the job-local mutex, then join without holding either
    // worker_mutex or downloads_mutex_. This avoids both data races on
    // std::thread and deadlocks with progress callbacks.
    if (!job) return;

    std::thread worker;
    {
        std::lock_guard<std::mutex> lock(job->worker_mutex);
        if (!job->worker.joinable()) return;
        if (job->worker.get_id() == std::this_thread::get_id()) return;
        worker = std::move(job->worker);
    }

    worker.join();
}

std::shared_ptr<Server::DownloadJob> Server::start_download_job(
    const std::string& download_id,
    const std::string& download_type,
    const std::string& display_name,
    std::function<void(DownloadProgressCallback)> operation) {

    std::shared_ptr<DownloadJob> old_job;
    auto job = std::make_shared<DownloadJob>();
    job->id = download_id;
    job->type = download_type;
    job->display_name = display_name;
    job->status = "downloading";
    job->running = true;
    job->progress = {
        {"id", download_id},
        {"type", download_type},
        {"model_name", display_name},
        {"file", ""},
        {"file_index", 0},
        {"total_files", 0},
        {"bytes_downloaded", 0},
        {"bytes_total", 0},
        {"total_download_size", 0},
        {"bytes_previously_downloaded", 0},
        {"completed_files_bytes", 0},
        {"cumulative_bytes_downloaded", 0},
        {"overall_bytes_downloaded", 0},
        {"percent", 0},
        {"complete", false}
    };

    std::unique_lock<std::mutex> worker_lock(job->worker_mutex);

    {
        std::lock_guard<std::mutex> lock(downloads_mutex_);
        auto existing = download_jobs_.find(download_id);
        if (existing != download_jobs_.end()) {
            if (existing->second->running || existing->second->status == "downloading") {
                return existing->second;
            }
            old_job = existing->second;
        }
        download_jobs_[download_id] = job;
    }

    join_download_job(old_job);

    job->worker = std::thread([this, job, operation = std::move(operation)]() mutable {
        try {
            DownloadProgressCallback progress_cb = [this, job](const DownloadProgress& p) -> bool {
                std::lock_guard<std::mutex> lock(downloads_mutex_);
                // Completion wins over a very late pause/cancel request. If the
                // downloader is reporting its final complete event, record it and
                // let the operation return successfully instead of turning a fully
                // written model into a cancelled/paused row.
                if (job->cancel_requested && !p.complete) {
                    job->stop_acknowledged = true;
                    return false;
                }

                if (job->current_file_index != p.file_index) {
                    if (job->current_file_index >= 0 && p.file_index > job->current_file_index) {
                        job->completed_files_bytes += job->current_file_bytes_total;
                    }
                    job->current_file_index = p.file_index;
                    job->current_file_bytes_total = 0;
                }
                if (p.bytes_total > 0) {
                    job->current_file_bytes_total = std::max<uint64_t>(
                        job->current_file_bytes_total,
                        static_cast<uint64_t>(p.bytes_total));
                }

                const uint64_t total_download_size = static_cast<uint64_t>(p.total_download_size);
                uint64_t cumulative_bytes = job->completed_files_bytes + static_cast<uint64_t>(p.bytes_downloaded);
                if (p.complete && total_download_size > 0) {
                    cumulative_bytes = total_download_size;
                } else if (total_download_size > 0) {
                    cumulative_bytes = std::min(cumulative_bytes, total_download_size);
                }

                job->progress = download_progress_to_json(p);
                job->progress["completed_files_bytes"] = job->completed_files_bytes;
                job->progress["cumulative_bytes_downloaded"] = cumulative_bytes;
                job->progress["overall_bytes_downloaded"] = cumulative_bytes;
                job->status = p.complete ? "completed" : "downloading";
                // terminal_since is the time the worker has actually stopped, not
                // the time a terminal status first becomes visible. Keeping it
                // empty while running=true prevents /downloads from expiring the
                // row before other tabs can observe that files are released.
                job->terminal_since = std::chrono::steady_clock::time_point{};
                job->error.clear();
                return true;
            };

            operation(progress_cb);

            std::lock_guard<std::mutex> lock(downloads_mutex_);
            if (job->cancel_requested && job->stop_acknowledged) {
                job->status = job->cancel_action == "cancel" ? "cancelled" : "paused";
                job->progress["complete"] = false;
            } else {
                // The operation returned normally without acknowledging a stop
                // request. Treat that as success; a late pause/cancel button must
                // not erase or delete a completed download.
                job->status = "completed";
                job->progress["complete"] = true;
                job->progress["percent"] = 100;
                const uint64_t total_download_size = job->progress.value("total_download_size", uint64_t{0});
                if (total_download_size > 0) {
                    job->progress["cumulative_bytes_downloaded"] = total_download_size;
                    job->progress["overall_bytes_downloaded"] = total_download_size;
                }
            }
            job->running = false;
            job->terminal_since = (job->status == "completed" || job->status == "cancelled")
                ? std::chrono::steady_clock::now()
                : std::chrono::steady_clock::time_point{};
        } catch (const lemon::UnknownModelError& e) {
            std::lock_guard<std::mutex> lock(downloads_mutex_);
            LOG(ERROR, "DownloadManager") << "worker unknown-model error id=" << job->id
                                           << " error=\"" << e.what() << "\"" << std::endl;
            job->status = "error";
            job->terminal_since = std::chrono::steady_clock::time_point{};
            job->error = e.what();
            job->progress["error"] = e.what();
            job->progress["code"] = lemon::kUnknownModelErrorCode;
            job->running = false;
        } catch (const std::exception& e) {
            bool cancel_requested = false;
            {
                std::lock_guard<std::mutex> lock(downloads_mutex_);
                cancel_requested = job->cancel_requested;
            }

            if (!cancel_requested) {
                LOG(ERROR, "DownloadManager") << "worker exception id=" << job->id
                                               << " error=\"" << e.what() << "\"" << std::endl;
            }

            std::lock_guard<std::mutex> lock(downloads_mutex_);
            if (job->cancel_requested) {
                job->status = job->cancel_action == "cancel" ? "cancelled" : "paused";
                job->error.clear();
            } else {
                job->status = "error";
                job->terminal_since = std::chrono::steady_clock::time_point{};
                job->error = e.what();
                job->progress["error"] = e.what();
            }
            job->running = false;
            if (job->status == "cancelled") {
                job->terminal_since = std::chrono::steady_clock::now();
            }
        }
    });
    worker_lock.unlock();

    return job;
}


// ============================================================================
// Shared SSE streaming helper for download operations
// ============================================================================

void Server::stream_download_operation(
    httplib::Response& res,
    std::function<void(DownloadProgressCallback)> operation) {

    res.set_header("Cache-Control", "no-cache");
    res.set_header("Connection", "keep-alive");
    res.set_header("X-Accel-Buffering", "no");

    res.set_chunked_content_provider(
        "text/event-stream",
        [operation = std::move(operation)](size_t offset, httplib::DataSink& sink) {
            if (offset > 0) {
                return false; // Already sent everything
            }

            try {
                // Create progress callback that emits SSE events
                bool complete_sent = false;
                DownloadProgressCallback progress_cb = [&sink, &complete_sent](const DownloadProgress& p) -> bool {
                    nlohmann::json event_data;
                    event_data["file"] = p.file;
                    event_data["file_index"] = p.file_index;
                    event_data["total_files"] = p.total_files;
                    event_data["bytes_downloaded"] = static_cast<uint64_t>(p.bytes_downloaded);
                    event_data["bytes_total"] = static_cast<uint64_t>(p.bytes_total);
                    event_data["total_download_size"] = static_cast<uint64_t>(p.total_download_size);
                    event_data["bytes_previously_downloaded"] = static_cast<uint64_t>(p.bytes_previously_downloaded);
                    event_data["percent"] = p.percent;

                    std::string event;
                    if (p.complete) {
                        event = "event: complete\ndata: " + event_data.dump() + "\n\n";
                        complete_sent = true;
                    } else {
                        event = "event: progress\ndata: " + event_data.dump() + "\n\n";
                    }

                    if (!sink.write(event.c_str(), event.size())) {
                        LOG(INFO, "Server") << "Client disconnected, cancelling download" << std::endl;
                        return false;
                    }
                    return true;
                };

                operation(progress_cb);

                // If operation completed without sending a "complete" event
                // (e.g. backend was already installed), send one now
                if (!complete_sent) {
                    nlohmann::json done_data = {{"status", "ok"}};
                    std::string event = "event: complete\ndata: " + done_data.dump() + "\n\n";
                    sink.write(event.c_str(), event.size());
                }

            } catch (const lemon::UnknownModelError& e) {
                nlohmann::json error_data = {{"error", e.what()}, {"code", lemon::kUnknownModelErrorCode}};
                std::string event = "event: error\ndata: " + error_data.dump() + "\n\n";
                sink.write(event.c_str(), event.size());
            } catch (const std::exception& e) {
                std::string error_msg = e.what();
                if (error_msg != "Download cancelled") {
                    nlohmann::json error_data = {{"error", error_msg}};
                    std::string event = "event: error\ndata: " + error_data.dump() + "\n\n";
                    sink.write(event.c_str(), event.size());
                }
            }

            sink.done();
            return false;
        });
}



void Server::handle_downloads(const httplib::Request&, httplib::Response& res) {
    nlohmann::json response = nlohmann::json::array();
    std::vector<std::shared_ptr<DownloadJob>> expired_jobs;
    const auto now = std::chrono::steady_clock::now();

    {
        std::lock_guard<std::mutex> lock(downloads_mutex_);
        for (auto it = download_jobs_.begin(); it != download_jobs_.end();) {
            const auto& job = it->second;
            if (is_download_job_visible(job)) {
                response.push_back(download_job_to_json(job));
                ++it;
                continue;
            }

            const bool expired_terminal = job &&
                !job->running &&
                job->status == "completed" &&
                job->terminal_since.time_since_epoch().count() > 0 &&
                now - job->terminal_since >= DOWNLOAD_TERMINAL_VISIBILITY;

            if (expired_terminal) {
                expired_jobs.push_back(job);
                it = download_jobs_.erase(it);
            } else {
                ++it;
            }
        }
    }

    for (auto& job : expired_jobs) {
        join_download_job(job);
    }

    res.set_content(response.dump(), "application/json");
}


void Server::handle_download_control(const httplib::Request& req, httplib::Response& res) {
    try {
        auto request_json = nlohmann::json::parse(req.body);
        std::string id = request_json.value("id", "");
        std::string action = request_json.value("action", "");

        if (id.empty() || action.empty()) {
            res.status = 400;
            res.set_content("{\"error\": \"Both 'id' and 'action' are required\"}", "application/json");
            return;
        }

        nlohmann::json response_json;
        std::shared_ptr<DownloadJob> job_to_join;

        {
            std::lock_guard<std::mutex> lock(downloads_mutex_);
            auto it = download_jobs_.find(id);
            if (it == download_jobs_.end()) {
                if (action == "remove") {
                    res.set_content("{\"status\": \"ok\", \"missing\": true}", "application/json");
                    return;
                }
                res.status = 404;
                res.set_content("{\"error\": \"Download not found\"}", "application/json");
                return;
            }

            auto job = it->second;
            if (action == "pause" || action == "cancel") {
                const bool terminal = job->status == "completed" ||
                    job->status == "cancelled" ||
                    job->status == "error";

                if (!terminal) {
                    job->cancel_requested = true;
                    job->cancel_action = action;
                    job->status = action == "cancel" ? "cancelled" : "paused";
                    // Paused jobs remain visible until resumed/removed. A cancel
                    // request for an already-stopped job has no worker that will
                    // later stamp terminal_since, so start the terminal visibility
                    // window here to avoid leaving a stale hidden registry entry.
                    job->terminal_since = (action == "cancel" && !job->running)
                        ? std::chrono::steady_clock::now()
                        : std::chrono::steady_clock::time_point{};
                }

                response_json = download_job_to_json(job);
            } else if (action == "remove") {
                if (job->running) {
                    // A remove request must not make the job disappear while the
                    // worker may still hold file handles. Convert it to a cancel
                    // request and keep the job visible until running=false.
                    job->cancel_requested = true;
                    job->cancel_action = "cancel";
                    if (job->status != "completed" && job->status != "error") {
                        job->status = "cancelled";
                        job->terminal_since = std::chrono::steady_clock::time_point{};
                    }
                    response_json = download_job_to_json(job);
                } else {
                    job_to_join = job;
                    download_jobs_.erase(it);
                    response_json = {{"status", "ok"}};
                }
            } else {
                res.status = 400;
                res.set_content("{\"error\": \"Unsupported download action\"}", "application/json");
                return;
            }
        }

        join_download_job(job_to_join);
        res.set_content(response_json.dump(), "application/json");
    } catch (const std::exception& e) {
        res.status = 400;
        nlohmann::json error = {{"error", e.what()}};
        res.set_content(error.dump(), "application/json");
    }
}

void Server::cancel_download_jobs() {
    std::vector<std::shared_ptr<DownloadJob>> jobs;
    {
        std::lock_guard<std::mutex> lock(downloads_mutex_);
        for (auto& [id, job] : download_jobs_) {
            job->cancel_requested = true;
            job->cancel_action = "cancel";
            jobs.push_back(job);
        }
    }

    for (auto& job : jobs) {
        join_download_job(job);
    }
}


// ============================================================================
// Backend management endpoints
// ============================================================================

void Server::handle_install(const httplib::Request& req, httplib::Response& res) {
    try {
        auto request_json = nlohmann::json::parse(req.body);

        // Cloud install branch. Cloud providers don't have a binary to fetch —
        // "installing" one means registering its OpenAI-compat base URL with
        // the in-process CloudProviderRegistry so that ModelManager can
        // discover its catalog as soon as the matching API key is supplied
        // (env var or POST /v1/cloud/auth). Shape:
        //   {backend: "cloud", provider: "fireworks",
        //    base_url: "https://api.fireworks.ai/inference/v1",
        //    allow_insecure_http: false,
        //    auth_header_name / auth_header_prefix / wire_format: see
        //      CloudProviderRegistry::Record,
        //    api_key: "..."}
        //
        // Every field after base_url is optional and applied only when
        // present, so a re-install that omits one (the desktop add-provider
        // form posts just backend / provider / base_url / api_key) keeps the
        // stored value.
        if (request_json.value("backend", "") == "cloud") {
            const std::string provider = request_json.value("provider", "");
            const std::string base_url = request_json.value("base_url", "");
            const std::string api_key = request_json.value("api_key", "");

            auto reject = [&](const std::string& message) {
                res.status = 400;
                nlohmann::json error = {{"error", {
                    {"message", message},
                    {"type", "invalid_request_error"}}}};
                res.set_content(error.dump(), "application/json");
            };
            if (provider.empty() || base_url.empty()) {
                reject("Cloud install requires 'provider' and 'base_url' string fields");
                return;
            }
            if (auto err = CloudProviderRegistry::validate_provider_name(provider); !err.empty()) {
                reject(err);
                return;
            }
            if (auto err = CloudProviderRegistry::validate_base_url(base_url); !err.empty()) {
                reject(err);
                return;
            }

            CloudProviderRegistry::InstallOptions install_options;
            auto read_validated_field = [&](const char* field,
                                            std::string (*validate)(const std::string&),
                                            std::optional<std::string>& out) {
                if (!request_json.contains(field)) return true;
                if (!request_json[field].is_string()) {
                    reject(std::string(field) + " must be a string when provided");
                    return false;
                }
                auto value = request_json[field].get<std::string>();
                if (auto err = validate(value); !err.empty()) {
                    reject(err);
                    return false;
                }
                out = std::move(value);
                return true;
            };
            if (!read_validated_field("auth_header_name",
                                      CloudProviderRegistry::validate_auth_header_name,
                                      install_options.auth_header_name) ||
                !read_validated_field("auth_header_prefix",
                                      CloudProviderRegistry::validate_auth_header_prefix,
                                      install_options.auth_header_prefix) ||
                !read_validated_field("wire_format",
                                      CloudProviderRegistry::validate_wire_format,
                                      install_options.wire_format)) {
                return;
            }
            if (request_json.contains("allow_insecure_http")) {
                if (!request_json["allow_insecure_http"].is_boolean()) {
                    reject("allow_insecure_http must be a boolean when provided");
                    return;
                }
                install_options.allow_insecure_http =
                    request_json["allow_insecure_http"].get<bool>();
            }
            // The http:// opt-in gate below reflects the effective state: an
            // omitted flag on a re-install keeps whatever was stored.
            const bool allow_insecure_http =
                install_options.allow_insecure_http.value_or(
                    cloud_registry_->allow_insecure_http_for(provider));

            const auto env_state = cloud_registry_->auth_state(provider);
            if (CloudProviderRegistry::is_http_base_url(base_url) &&
                !allow_insecure_http &&
                (!api_key.empty() || env_state.env_var_set)) {
                res.status = 400;
                nlohmann::json error = {{"error", {
                    {"message", "Cloud provider '" + provider + "' uses http://. "
                                "Set allow_insecure_http=true to explicitly opt in before "
                                "storing or using an API key over plaintext HTTP."},
                    {"type", "invalid_request_error"},
                    {"code", "insecure_http_requires_opt_in"}}}};
                res.set_content(error.dump(), "application/json");
                return;
            }
            LOG(INFO, "Server") << "Installing cloud provider '" << provider
                                  << "' with base_url " << base_url << std::endl;
            cloud_registry_->install(provider, base_url, install_options);
            persist_cloud_providers();

            // Best-effort optional auth: if api_key was supplied, treat this
            // as "install + auth in one shot". Honors the env-wins rule: if
            // LEMONADE_<P>_API_KEY is set, we don't store the runtime key and
            // we don't error — install still succeeds, env var still wins.
            bool runtime_key_stored = false;
            if (!api_key.empty()) {
                runtime_key_stored = cloud_registry_->set_runtime_key(provider, api_key);
            }

            // Best-effort discovery. Empty result is fine — install means
            // "registered", not "verified-and-non-empty". The client can
            // call /v1/system-info later to see how many models showed up.
            size_t models_after = model_manager_->refresh_cloud_models(provider);
            const auto state = cloud_registry_->auth_state(provider);
            const auto auth_header = cloud_registry_->auth_header_for(provider);

            nlohmann::json response = {
                {"status", "success"},
                {"backend", "cloud"},
                {"provider", provider},
                {"base_url", cloud_registry_->base_url_for(provider)},
                {"allow_insecure_http", cloud_registry_->allow_insecure_http_for(provider)},
                {"auth_header_name", auth_header.name},
                {"auth_header_prefix", auth_header.prefix},
                {"wire_format", cloud_registry_->wire_format_for(provider)},
                {"models_discovered", models_after},
                {"auth_state", {
                    {"env_var_set", state.env_var_set},
                    {"runtime_key_set", state.runtime_key_set}
                }}
            };
            std::vector<std::string> warnings = CloudProviderRegistry::base_url_warnings(
                cloud_registry_->base_url_for(provider),
                state.env_var_set || state.runtime_key_set);
            if (!api_key.empty() && !runtime_key_stored) {
                warnings.push_back(CloudProviderRegistry::env_var_name(provider) +
                    " is set; supplied api_key was ignored.");
            }
            attach_warnings(response, warnings);
            res.set_content(response.dump(), "application/json");
            return;
        }

        std::string recipe = request_json.value("recipe", "");
        std::string backend = request_json.value("backend", "");
        bool stream = request_json.value("stream", false);
        bool subscribe = request_json.value("subscribe", true);
        bool force = request_json.value("force", false);

        if (recipe.empty() || backend.empty()) {
            res.status = 400;
            nlohmann::json error = {{"error", "Both 'recipe' and 'backend' are required"}};
            res.set_content(error.dump(), "application/json");
            return;
        }

        LOG(INFO, "Server") << "Installing backend: " << recipe << ":" << backend << std::endl;

        // Get fresh state before any checks
        SystemInfoCache::invalidate_recipes();

        // Check if this backend requires manual setup (e.g. FLM on Linux).
        // If so, return the action URL instead of attempting installation.
        json system_info = SystemInfoCache::get_system_info_with_cache();
        if (system_info.contains("recipes") &&
            system_info["recipes"].contains(recipe) &&
            system_info["recipes"][recipe].contains("backends") &&
            system_info["recipes"][recipe]["backends"].contains(backend)) {
            const auto& backend_info = system_info["recipes"][recipe]["backends"][backend];
            std::string state = backend_info.value("state", "unsupported");
            std::string message = backend_info.value("message", "Backend is not supported on this system.");
            std::string action = backend_info.value("action", "");

            if (state == "unsupported" && !force) {
                res.status = 400;
                nlohmann::json error = {
                    {"error", "Cannot install " + recipe + ":" + backend + " on this system: " + message},
                    {"recipe", recipe},
                    {"backend", backend}
                };
                res.set_content(error.dump(), "application/json");
                return;
            }

            if (action.find(".html") != std::string::npos) {
                auto url_pos = action.find("https://");
                if (url_pos != std::string::npos) {
                    nlohmann::json response = {
                        {"action", action.substr(url_pos)},
                        {"recipe", recipe},
                        {"backend", backend}
                    };
                    res.set_content(response.dump(), "application/json");
                    return;
                }
            }
        }

        if (stream) {
            auto operation = [this, recipe, backend, force](DownloadProgressCallback progress_cb) {
                backend_manager_->install_backend(recipe, backend, force, progress_cb);
                SystemInfoCache::invalidate_recipes();
                model_manager_->invalidate_models_cache();
            };

            if (!subscribe) {
                // Server-owned mode for desktop UI reload/new-tab recovery.
                // Legacy streamed /install behavior is unchanged.
                const std::string display_name = recipe + ":" + backend;
                auto job = start_download_job("backend:" + display_name, "backend", display_name, operation);
                nlohmann::json response;
                {
                    std::lock_guard<std::mutex> lock(downloads_mutex_);
                    response = download_job_to_json(job);
                }
                res.set_content(response.dump(), "application/json");
                return;
            }

            stream_download_operation(res, operation);
        } else {
            backend_manager_->install_backend(recipe, backend, force);
            SystemInfoCache::invalidate_recipes();
            model_manager_->invalidate_models_cache();
            nlohmann::json response = {
                {"status", "success"},
                {"recipe", recipe},
                {"backend", backend}
            };
            res.set_content(response.dump(), "application/json");
        }

    } catch (const std::exception& e) {
        LOG(ERROR, "Server") << "ERROR in handle_install: " << e.what() << std::endl;
        res.status = 500;
        nlohmann::json error = {{"error", e.what()}};
        res.set_content(error.dump(), "application/json");
    }
}

void Server::handle_install_dry_run(const httplib::Request& req, httplib::Response& res) {
    // Resolve the backend asset URL that install_backend() would download for a
    // given recipe/backend, optionally for a mocked GPU arch, without fetching
    // any bytes — so a 404 from a stale target-name or version pin can be caught
    // for any arch without that GPU present.
    std::string requested_arch;
    try {
        auto request_json = nlohmann::json::parse(req.body);

        const std::string recipe = request_json.value("recipe", "");
        const std::string backend = request_json.value("backend", "");
        requested_arch = request_json.value("arch", "");

        if (recipe.empty() || backend.empty()) {
            res.status = 400;
            nlohmann::json error = {{"error", "Both 'recipe' and 'backend' are required"}};
            res.set_content(error.dump(), "application/json");
            return;
        }

        if (!requested_arch.empty()) {
            SystemInfo::set_rocm_arch_override(requested_arch);
        }

        auto params = backend_manager_->get_install_params(recipe, backend);

        // Clear the override as soon as resolution is done so it cannot leak to
        // any later work on this thread.
        SystemInfo::set_rocm_arch_override("");

        const std::string url = "https://github.com/" + params.repo +
                                "/releases/download/" + params.version + "/" +
                                params.filename;

        bool supports_split_archive = false;
        if (auto* spec = backends::try_get_spec_for_recipe(recipe)) {
            supports_split_archive = spec->supports_split_archive;
        }

        bool supported = requested_arch.empty()
            ? true
            : SystemInfo::backend_supports_arch(recipe, backend, requested_arch);

        nlohmann::json response = {
            {"recipe", recipe},
            {"backend", backend},
            {"arch", requested_arch},
            {"repo", params.repo},
            {"version", params.version},
            {"filename", params.filename},
            {"url", url},
            {"supports_split_archive", supports_split_archive},
            {"supported", supported}
        };
        res.set_content(response.dump(), "application/json");
    } catch (const std::exception& e) {
        SystemInfo::set_rocm_arch_override("");
        res.status = 500;
        nlohmann::json error = {{"error", e.what()}};
        if (!requested_arch.empty()) {
            error["arch"] = requested_arch;
        }
        res.set_content(error.dump(), "application/json");
    }
}

void Server::handle_uninstall(const httplib::Request& req, httplib::Response& res) {
    try {
        auto request_json = nlohmann::json::parse(req.body);

        // Cloud uninstall: drop the provider record + its runtime key +
        // every discovered model. Idempotent — uninstalling an unknown
        // provider returns 404 (matching the symmetric install error shape)
        // rather than silently succeeding, so CI scripts can tell which
        // case happened.
        if (request_json.value("backend", "") == "cloud") {
            const std::string provider = request_json.value("provider", "");
            if (provider.empty()) {
                res.status = 400;
                nlohmann::json error = {{"error", {
                    {"message", "Cloud uninstall requires 'provider' string field"},
                    {"type", "invalid_request_error"}}}};
                res.set_content(error.dump(), "application/json");
                return;
            }
            // Unload any loaded cloud-recipe models for this provider before
            // erasing them from the cache. Router::unload_model expects a
            // model_name, not a recipe filter — walk the loaded list manually.
            auto loaded = router_->get_all_loaded_models();
            for (const auto& m : loaded) {
                if (m.value("recipe", "") == "cloud" &&
                    m.value("cloud_provider", "") == provider) {
                    router_->unload_model(m.value("model_name", ""));
                }
            }
            bool removed = cloud_registry_->uninstall(provider);
            size_t evicted = model_manager_->evict_cloud_models(provider);
            if (!removed) {
                res.status = 404;
                nlohmann::json error = {{"error", {
                    {"message", "Cloud provider '" + provider + "' is not installed"},
                    {"type", "invalid_request_error"}}}};
                res.set_content(error.dump(), "application/json");
                return;
            }
            persist_cloud_providers();
            nlohmann::json response = {
                {"status", "success"},
                {"backend", "cloud"},
                {"provider", provider},
                {"models_evicted", evicted}
            };
            res.set_content(response.dump(), "application/json");
            return;
        }

        std::string recipe = request_json.value("recipe", "");
        std::string backend = request_json.value("backend", "");

        if (recipe.empty() || backend.empty()) {
            res.status = 400;
            nlohmann::json error = {{"error", "Both 'recipe' and 'backend' are required"}};
            res.set_content(error.dump(), "application/json");
            return;
        }

        LOG(INFO, "Server") << "Uninstalling backend: " << recipe << ":" << backend << std::endl;

        // Check if any loaded models use this recipe+backend and unload them first
        auto loaded_models = router_->get_all_loaded_models();
        std::string backend_option_key = recipe + "_backend";
        for (const auto& model : loaded_models) {
            if (model.value("recipe", "") == recipe) {
                // Check if the model's backend matches the one being uninstalled
                std::string model_backend;
                if (model.contains("recipe_options") && model["recipe_options"].contains(backend_option_key)) {
                    model_backend = model["recipe_options"].value(backend_option_key, "");
                }
                if (!model_backend.empty() && model_backend != backend) {
                    continue;  // Different backend, skip
                }
                std::string model_name = model.value("model_name", "");
                LOG(INFO, "Server") << "Unloading model " << model_name
                          << " before uninstalling " << recipe << ":" << backend << std::endl;
                router_->unload_model(model_name);
            }
        }

        backend_manager_->uninstall_backend(recipe, backend);

        SystemInfoCache::invalidate_recipes();
        model_manager_->invalidate_models_cache();

        nlohmann::json response = {
            {"status", "success"},
            {"recipe", recipe},
            {"backend", backend}
        };
        res.set_content(response.dump(), "application/json");

    } catch (const std::exception& e) {
        LOG(ERROR, "Server") << "ERROR in handle_uninstall: " << e.what() << std::endl;
        res.status = 500;
        nlohmann::json error = {{"error", e.what()}};
        res.set_content(error.dump(), "application/json");
    }
}

void Server::enrich_recipes(json& recipes) {
    if (!backend_manager_) return;

    for (auto& [recipe_name, recipe_info] : recipes.items()) {
        if (!recipe_info.contains("backends")) continue;
        for (auto& [backend_name, backend_info] : recipe_info["backends"].items()) {
            try {
                auto enrichment = backend_manager_->get_backend_enrichment(recipe_name, backend_name);
                if (!enrichment.release_url.empty()) {
                    backend_info["release_url"] = enrichment.release_url;
                }
                if (!enrichment.download_filename.empty()) {
                    backend_info["download_filename"] = enrichment.download_filename;
                }
                if (!backend_info.contains("version") || backend_info["version"].get<std::string>().empty()) {
                    if (!enrichment.version.empty()) {
                        backend_info["version"] = enrichment.version;
                    }
                }
            } catch (...) {}
        }
    }
}

} // namespace lemon
