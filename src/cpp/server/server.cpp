#include "lemon/server.h"
#include <optional>
#include "lemon/collection_orchestrator.h"
#include "lemon/hf_variants.h"
#include "lemon/model_registry.h"
#include "lemon/route_decision_response.h"
#include "lemon/routing_classifier_services.h"
#include "lemon/routing_policy.h"
#include "lemon/config_file.h"
#include "lemon/mcp_server.h"
#include "lemon/ollama_api.h"
#include "lemon/backends/cloud/cloud_server.h"
#include "lemon/backends/sdcpp/sdcpp_server.h"
#include "lemon/backends/backend_utils.h"
#include <cstring>
#include "lemon/utils/image_sniff.h"
#include "lemon/utils/json_utils.h"
#include "lemon/utils/path_utils.h"
#include "lemon/streaming_proxy.h"
#include "lemon/logging_config.h"
#include "lemon/prometheus_metrics.h"
#include "lemon/runtime_config.h"
#include "telemetry.h"
#include "lemon/system_info.h"
#include "lemon/version.h"
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <map>
#include <memory>
#include <thread>
#include <chrono>
#include <mutex>
#include <filesystem>
#include <system_error>
#include <algorithm>
#include <cmath>
#include <set>
#include <vector>
#include <lemon/utils/aixlog.hpp>
#include "lemon/utils/network_utils.h"

#ifdef _WIN32
    #include <windows.h>
    #include <winsock2.h>
    #include <ws2tcpip.h>
#else
    #include <arpa/inet.h>   // inet_pton, htons
    #include <fcntl.h>
    #include <netdb.h>       // Crucial for getaddrinfo and addrinfo struct
    #include <netinet/in.h>  // sockaddr_in / sockaddr_in6
    #include <sys/socket.h>
    #include <sys/types.h>
    #include <unistd.h>
#endif

#ifdef __APPLE__
    #include <sys/sysctl.h>
#endif

#ifdef __linux__
    #include <sys/ioctl.h>
    #include <fcntl.h>
    #include <unistd.h>
    #include <libdrm/drm.h>
    #include "lemon/amdxdna_accel.h"
#endif

namespace fs = std::filesystem;

namespace lemon {

namespace {

bool should_disable_thinking(const json& request_json) {
    // enable_thinking takes precedence over thinking when both are present.
    if (request_json.contains("enable_thinking") && request_json["enable_thinking"].is_boolean()) {
        return request_json["enable_thinking"].get<bool>() == false;
    }

    if (request_json.contains("thinking")) {
        const auto& thinking = request_json["thinking"];
        if (thinking.is_boolean()) {
            return thinking.get<bool>() == false;
        }
        if (thinking.is_object()) {
            const std::string type = thinking.value("type", "");
            if (type == "disabled") {
                return true;
            }
            if (type == "enabled") {
                return false;
            }
        }
    }

    return false;
}

bool strip_handled_thinking_fields(json& request_json) {
    bool modified = false;
    modified = request_json.erase("enable_thinking") > 0 || modified;
    modified = request_json.erase("thinking") > 0 || modified;
    return modified;
}

// Normalize client-provided model names: strip ":latest" suffix (Ollama/Docker convention)
// Returns true if the model name was modified
bool normalize_client_model_name(json& request_json) {
    if (!request_json.contains("model") || !request_json["model"].is_string()) {
        return false;
    }

    std::string model_name = request_json["model"].get<std::string>();
    const std::string latest_suffix = ":latest";

    if (model_name.size() > latest_suffix.size() &&
        model_name.substr(model_name.size() - latest_suffix.size()) == latest_suffix) {
        std::string normalized = model_name.substr(0, model_name.size() - latest_suffix.size());
        request_json["model"] = normalized;
        return true;
    }

    return false;
}

bool prepend_no_think_to_last_user_message(json& request_json) {
    if (!request_json.contains("messages") || !request_json["messages"].is_array()) {
        LOG(DEBUG, "Server") << "No messages array found for /no_think injection" << std::endl;
        return false;
    }

    auto& messages = request_json["messages"];

    for (int i = static_cast<int>(messages.size()) - 1; i >= 0; i--) {
        if (messages[i].is_object() &&
            messages[i].contains("role") &&
            messages[i]["role"].is_string() &&
            messages[i]["role"].get<std::string>() == "user" &&
            messages[i].contains("content") &&
            messages[i]["content"].is_string()) {

            std::string original_content = messages[i]["content"].get<std::string>();
            messages[i]["content"] = "/no_think\n" + original_content;
            return true;
        }
    }

    LOG(DEBUG, "Server") << "No string-content user message found for /no_think injection" << std::endl;
    return false;
}

bool valid_error_status(int status_code) {
    return status_code >= 400 && status_code <= 599;
}

int get_error_status_code(const json& response, int default_status_code = 500) {
    if (!response.contains("error") || !response["error"].is_object()) {
        return default_status_code;
    }

    const auto& error = response["error"];
    if (error.contains("status_code") && error["status_code"].is_number_integer()) {
        int status_code = error["status_code"].get<int>();
        if (valid_error_status(status_code)) {
            return status_code;
        }
    }

    if (error.contains("details") && error["details"].is_object()) {
        const auto& details = error["details"];
        if (details.contains("status_code") && details["status_code"].is_number_integer()) {
            int status_code = details["status_code"].get<int>();
            if (valid_error_status(status_code)) {
                return status_code;
            }
        }
    }

    return default_status_code;
}

void set_error_response(const json& response, httplib::Response& res,
                        int default_status_code = 500) {
    res.status = get_error_status_code(response, default_status_code);
    res.set_content(response.dump(), "application/json");
}

void attach_route_decision(json& response, httplib::Response& res,
                           const std::optional<RouterDispatchResult>& dispatch) {
    if (!dispatch.has_value()) {
        return;
    }
    response["x_lemonade_route"] = route_decision_to_json(dispatch->decision);
    attach_route_header(res, dispatch->decision);
}

template <typename StreamFn>
void set_route_decision_sse_content_provider(
    httplib::Response& res,
    const std::optional<RouterDispatchResult>& dispatch,
    std::string request_body,
    StreamFn stream_fn) {
    res.set_header("Cache-Control", "no-cache");
    res.set_header("Connection", "keep-alive");
    res.set_header("X-Accel-Buffering", "no");
    if (dispatch) {
        attach_route_header(res, dispatch->decision);
    }

    json route_decision_json = dispatch
        ? route_decision_to_json(dispatch->decision)
        : json(nullptr);
    res.set_chunked_content_provider(
        "text/event-stream",
        [request_body = std::move(request_body),
         route_decision_json = std::move(route_decision_json),
         stream_fn = std::move(stream_fn)](size_t offset, httplib::DataSink& sink) {
            if (offset > 0) {
                return false;
            }

            stream_with_route_decision(
                sink,
                route_decision_json,
                [&request_body, &stream_fn](httplib::DataSink& route_sink) {
                    stream_fn(request_body, route_sink);
                });
            return false;
        });
}

int get_http_status_from_error(const std::string& error_code) {
    if (error_code == "slots_pinned_error") {
        return 409;
    } else if (error_code == "model_load_error") {
        return 500;
    } else {
        return 404;
    }
}

bool is_quiet_polling_path(const std::string& path) {
    return path == "/api/v0/downloads" || path == "/api/v1/downloads" ||
           path == "/v0/downloads" || path == "/v1/downloads" ||
           path == "/api/v0/system-stats" || path == "/api/v1/system-stats" ||
           path == "/v0/system-stats" || path == "/v1/system-stats" ||
           path == "/api/v0/stats" || path == "/api/v1/stats" ||
           path == "/v0/stats" || path == "/v1/stats";
}

std::string join_warnings(const std::vector<std::string>& warnings) {
    std::ostringstream joined;
    for (size_t i = 0; i < warnings.size(); ++i) {
        if (i > 0) {
            joined << " | ";
        }
        joined << warnings[i];
    }
    return joined.str();
}

void attach_warnings(json& response, const std::vector<std::string>& warnings) {
    if (warnings.empty()) {
        return;
    }
    response["warnings"] = warnings;
    // Backward-compatible single-string field for older clients.
    response["warning"] = join_warnings(warnings);
}

nlohmann::json get_model_storage_stats(const std::string& model_storage_path) {
    auto make_error_result = [](const fs::path& path, const std::string& error) {
        return nlohmann::json{
            {"path", utils::path_to_utf8(path)},
            {"used_bytes", nullptr},
            {"total_bytes", nullptr},
            {"free_bytes", nullptr},
            {"error", error}
        };
    };

    std::error_code ec;
    fs::path configured_path;

    if (!model_storage_path.empty()) {
        configured_path = utils::path_from_utf8(model_storage_path);
    }

    if (configured_path.empty()) {
        configured_path = fs::current_path(ec);
        if (ec) {
            LOG(WARNING, "Server") << "Unable to resolve current path for model storage stats: "
                                   << ec.message() << std::endl;
            return make_error_result(
                fs::path{},
                "Unable to resolve current path: " + ec.message()
            );
        }
    } else if (configured_path.is_relative()) {
        configured_path = fs::absolute(configured_path, ec);
        if (ec) {
            LOG(WARNING, "Server") << "Unable to resolve model storage path "
                                   << model_storage_path << ": " << ec.message() << std::endl;
            return make_error_result(
                configured_path,
                "Unable to resolve model storage path: " + ec.message()
            );
        }
    }

    configured_path = configured_path.lexically_normal();

    fs::path probe_path = configured_path;
    while (!probe_path.empty()) {
        std::error_code exists_ec;
        if (fs::exists(probe_path, exists_ec)) {
            break;
        }

        if (exists_ec) {
            LOG(WARNING, "Server") << "Unable to inspect model storage path "
                                   << utils::path_to_utf8(probe_path) << ": "
                                   << exists_ec.message() << std::endl;
            return make_error_result(
                configured_path,
                "Unable to inspect model storage path: " + exists_ec.message()
            );
        }

        fs::path parent_path = probe_path.parent_path();
        if (parent_path == probe_path) {
            break;
        }

        probe_path = parent_path;
    }

    auto space_info = fs::space(probe_path, ec);
    if (ec) {
        LOG(WARNING, "Server") << "Unable to read model storage stats for "
                               << utils::path_to_utf8(probe_path) << ": "
                               << ec.message() << std::endl;
        return make_error_result(
            configured_path,
            "Unable to read model storage stats: " + ec.message()
        );
    }

    const uintmax_t total_bytes = space_info.capacity;
    const uintmax_t free_bytes = std::min(space_info.available, space_info.capacity);
    const uintmax_t used_bytes = total_bytes - free_bytes;

    return nlohmann::json{
        {"path", utils::path_to_utf8(configured_path)},
        {"used_bytes", static_cast<uint64_t>(used_bytes)},
        {"total_bytes", static_cast<uint64_t>(total_bytes)},
        {"free_bytes", static_cast<uint64_t>(free_bytes)}
    };
}

} // namespace


static const json MIME_TYPES = {
    {"mp3",  "audio/mpeg"},
    {"opus", "audio/opus"},
    {"aac",  "audio/aac"},
    {"flac", "audio/flac"},
    {"wav",  "audio/wav"},
    {"pcm",  "audio/l16;rate=24000;endianness=little-endian"}
};

Server::Server(std::shared_ptr<RuntimeConfig> config, const std::string& cache_dir)
    : config_(config),
      cache_dir_(cache_dir),
      port_(config->port()), running_(false), udp_beacon_(),
      metrics_platform_(create_metrics_platform()) {

    // Set global HttpClient timeout
    utils::HttpClient::set_default_timeout(config->global_timeout());

    cloud_registry_ = std::make_unique<CloudProviderRegistry>();
    // Seed installed providers from config.json. Runtime keys stay empty
    // until either an env var resolves them per-request or a client POSTs
    // /v1/cloud/auth — by design we never persist secrets to disk.
    {
        json snap = config_->snapshot();
        if (snap.contains("cloud_providers")) {
            cloud_registry_->load_from_config(snap["cloud_providers"]);
        }
    }

    model_manager_ = std::make_unique<ModelManager>(config_->extra_models_dir());
    model_manager_->set_cloud_registry(cloud_registry_.get());

    backend_manager_ = std::make_unique<BackendManager>();
    BackendManager::set_global(backend_manager_.get());

    router_ = std::make_unique<Router>(config_.get(),
                                       model_manager_.get(),
                                       backend_manager_.get());
    router_->set_cloud_registry(cloud_registry_.get());

    LOG(DEBUG, "Server") << "Debug logging enabled - subprocess output will be visible" << std::endl;

    const char* api_key_env = std::getenv("LEMONADE_API_KEY");
    api_key_ = api_key_env ? std::string(api_key_env) : "";

    // Read admin API key - if not set, defaults to regular API key value
    const char* admin_api_key_env = std::getenv("LEMONADE_ADMIN_API_KEY");
    if (admin_api_key_env) {
        admin_api_key_ = std::string(admin_api_key_env);
    } else {
        admin_api_key_ = api_key_;
    }

    setup_http_servers();

    // Initialize WebSocket server for realtime API and log streaming
    websocket_server_ = std::make_unique<WebSocketServer>(
        router_.get(),
        config_->host(),
        config_->websocket_port());

    start_model_cache_warmup();
}

void Server::start_model_cache_warmup() {
    if (model_cache_warmup_thread_.joinable()) {
        return;
    }

    model_cache_warmup_thread_ = std::thread([this]() {
        try {
            LOG(DEBUG, "Server") << "Warming model list cache..." << std::endl;
            model_manager_->get_supported_models();
            LOG(DEBUG, "Server") << "Model list cache warmup complete" << std::endl;
        } catch (const std::exception& e) {
            LOG(WARNING, "Server") << "Model list cache warmup failed: " << e.what() << std::endl;
        } catch (...) {
            LOG(WARNING, "Server") << "Model list cache warmup failed with unknown error" << std::endl;
        }

        if (config_->auto_check_model_updates()) {
            try {
                LOG(DEBUG, "Server") << "Checking downloaded models for updates..." << std::endl;
                (void)model_manager_->check_for_model_updates();
                LOG(DEBUG, "Server") << "Model update check complete" << std::endl;
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

// Extract the member-function pointer for httplib::Server's private virtual
// process_and_close_socket (see upgradable_http_server.h). Explicit
// instantiation is the one context where C++ permits naming a private member.
template struct lemon::detail::PrivateMemberInit<
    lemon::detail::ProcessAndCloseSocketTag,
    &httplib::Server::process_and_close_socket>;

void Server::setup_http_servers() {
    http_server_ = std::make_unique<RoutedHttpServer>();
    http_server_v6_ = std::make_unique<RoutedHttpServer>();

    // Front listeners for the main port: WebSocket upgrades for /realtime and
    // /logs/stream are adopted by the libwebsockets server; everything else is
    // processed by the routed servers above. The dedicated websocket_port
    // listener keeps running unchanged.
    auto upgrade_handler = [this](socket_t sock) -> bool {
        if (websocket_server_ && websocket_server_->is_running()) {
            return websocket_server_->adopt_socket(static_cast<intptr_t>(sock));
        }
        return false;
    };
    http_front_ = std::make_unique<UpgradableFrontServer>(http_server_.get(), upgrade_handler);
    http_front_v6_ = std::make_unique<UpgradableFrontServer>(http_server_v6_.get(), upgrade_handler);

    // Keep cpp-httplib's default socket options here. httplib binds IPv6 with
    // IPV6_V6ONLY=0, so "::" overlaps the IPv4 wildcard "0.0.0.0" and only the
    // default SO_REUSEPORT lets the two coexist. Duplicate detection is done by
    // port_is_available() in run(), not by making these listeners exclusive.

    // CRITICAL: Enable multi-threading so the server can handle concurrent requests
    // Without this, the server is single-threaded and blocks on long operations

    // Size the pool from the host CPU count instead of a fixed 8. cpp-httplib
    // dedicates one worker thread per in-flight request for the connection's
    // lifetime, so a small fixed pool lets a handful of slow-loris or long-lived
    // streaming connections starve the management endpoints (/health, /load).
    unsigned int hw = std::thread::hardware_concurrency();
    size_t thread_count = std::clamp<size_t>(static_cast<size_t>(hw) * 4, 32, 256);
    std::function<httplib::TaskQueue *(void)> task_queue_factory = [thread_count] {
        LOG(DEBUG, "Server") << "Creating new thread pool with " << thread_count
                             << " threads" << std::endl;
        return new httplib::ThreadPool(thread_count);
    };

    // The fronts own the accept loops (and therefore the task queues)
    http_front_->new_task_queue = task_queue_factory;
    http_front_v6_->new_task_queue = task_queue_factory;
    http_server_->new_task_queue = task_queue_factory;
    http_server_v6_->new_task_queue = task_queue_factory;

    // Bound how long a single connection can tie up a worker thread. Without a
    // read timeout a client that opens a socket and never finishes its request
    // (slow loris) holds its worker indefinitely. Streaming responses drive
    // their own write cadence, so keep the write timeout generous. The fronts
    // need the same limits: they own accept and run the WebSocket upgrade peek
    // before delegating, so a stalled client could otherwise hold a front
    // worker there.
    for (httplib::Server* srv : {static_cast<httplib::Server*>(http_front_.get()),
                                 static_cast<httplib::Server*>(http_front_v6_.get()),
                                 static_cast<httplib::Server*>(http_server_.get()),
                                 static_cast<httplib::Server*>(http_server_v6_.get())}) {
        srv->set_read_timeout(30, 0);
        srv->set_write_timeout(300, 0);
        srv->set_keep_alive_max_count(100);
    }

    setup_routes(*http_server_);
    setup_routes(*http_server_v6_);
}

void Server::stop_http_listeners() {
    // The routed servers never own the listen socket: clear the injected fd so
    // their per-connection keep-alive loops exit, then close it once via the
    // fronts (which are the servers actually listening).
    if (http_server_) {
        http_server_->set_listen_socket(INVALID_SOCKET);
    }
    if (http_server_v6_) {
        http_server_v6_->set_listen_socket(INVALID_SOCKET);
    }
    if (http_front_) {
        http_front_->stop();
    }
    if (http_front_v6_) {
        http_front_v6_->stop();
    }
}

Server::~Server() {
    cancel_download_jobs();
    stop();
}

void Server::log_request(const httplib::Request& req) {
    if (req.path != "/api/v0/health" && req.path != "/api/v1/health" &&
        req.path != "/v0/health" && req.path != "/v1/health" &&
        req.path != "/live" &&
        req.path != "/metrics" &&
        !is_quiet_polling_path(req.path)) {
        LOG(DEBUG, "Server") << req.method << " " << req.path << std::endl;
    }
}

httplib::Server::HandlerResponse Server::authenticate_request(const httplib::Request& req, httplib::Response& res) {
    telemetry::g_request_start_time = std::chrono::steady_clock::now();
    telemetry::g_current_auth_token = "";
    if (req.has_header("X-Client-Session-Id")) {
        telemetry::g_current_client_session_id = req.get_header_value("X-Client-Session-Id");
    } else {
        telemetry::g_current_client_session_id.clear();
    }

    // Check if path requires authentication (API routes and internal endpoints).
    // /mcp is included here so that LEMONADE_API_KEY enforcement covers the MCP
    // gateway (Critical Invariant #10). It is the only API route outside the
    // /api/, /v0/, /v1/ prefixes — see register_routes() in McpServer for why.
    bool is_api_route = (req.path.rfind("/api/", 0) == 0) ||
                        (req.path.rfind("/v0/", 0) == 0) ||
                        (req.path.rfind("/v1/", 0) == 0) ||
                        (req.path == "/mcp");
    bool is_internal_route = (req.path.rfind("/internal/", 0) == 0);
    bool is_metrics_route = (req.path == "/metrics");

    // Authentication hierarchy. Two credentials gate two classes of endpoints:
    // api_key_ gates the regular API endpoints (/api, /v0, /v1); admin_api_key_
    // gates the internal control endpoints (/internal/*). admin_api_key_ defaults
    // to api_key_ when LEMONADE_ADMIN_API_KEY is unset.
    // - admin_api_key_ authenticates against both regular and internal endpoints.
    // - api_key_ authenticates against the regular endpoints only. It cannot
    //   reach /internal/* when LEMONADE_ADMIN_API_KEY is set to a distinct value;
    //   when LEMONADE_ADMIN_API_KEY is unset, admin_api_key_ == api_key_, so the
    //   regular key also authenticates against /internal/*.
    // - If api_key_ is empty, the regular endpoints require no authentication.
    // - If admin_api_key_ is empty (neither key set), /internal/* requires none.

    // Safely extract bearer token, guarding against malformed Authorization headers
    std::string auth_token;
    try {
        if (req.has_header("Authorization")) {
            auto auth_value = req.get_header_value("Authorization");
            // httplib::get_bearer_token_auth does substr(7) for "Bearer ", so check length
            if (auth_value.size() >= 7) {
                auth_token = httplib::get_bearer_token_auth(req);
            }
            // Silently ignore malformed/short Authorization headers
        }
    } catch (const std::exception& e) {
        LOG(DEBUG, "Server") << "Failed to parse Authorization header: " << e.what() << std::endl;
    }

    telemetry::g_current_auth_token = auth_token;

    if (is_internal_route) {
        // Internal routes require admin key authentication
        if (!admin_api_key_.empty() && req.method != "OPTIONS") {
            if (auth_token != admin_api_key_) {
                res.status = 401;
                res.set_content("{\"error\": \"Invalid or missing admin API key\"}", "application/json");
                return httplib::Server::HandlerResponse::Handled;
            }
        }
    } else if ((is_api_route || is_metrics_route) && req.method != "OPTIONS") {
        if (!api_key_.empty()) {
            if ((auth_token != api_key_) && (auth_token != admin_api_key_)) {
                res.status = 401;
                res.set_content("{\"error\": \"Invalid or missing API key\"}", "application/json");
                return httplib::Server::HandlerResponse::Handled;
            }
        }
    }

    return httplib::Server::HandlerResponse::Unhandled;
}


void Server::setup_routes(httplib::Server &web_server) {
    // Add pre-routing handler to log ALL incoming requests (except health checks)
    web_server.set_pre_routing_handler([this](const httplib::Request& req, httplib::Response& res) {
        this->log_request(req);
        return authenticate_request(req, res);
    });

    web_server.Get("/live", [this](const httplib::Request& req, httplib::Response& res) {
        handle_live(req, res);
    });

    // Prometheus scrape endpoint for Lemonade, model, backend, and system metrics.
    web_server.Get("/metrics", [this](const httplib::Request& req, httplib::Response& res) {
        handle_metrics(req, res);
    });

    // Setup CORS for all routes
    setup_cors(web_server);

    // Helper lambda to register routes for both v0 and v1 (with and without /api prefix for OpenAI compatibility)
    auto register_get = [this, &web_server](const std::string& endpoint,
                               std::function<void(const httplib::Request&, httplib::Response&)> handler) {
        web_server.Get("/api/v0/" + endpoint, handler);
        web_server.Get("/api/v1/" + endpoint, handler);
        web_server.Get("/v0/" + endpoint, handler);
        web_server.Get("/v1/" + endpoint, handler);
    };

    auto register_post = [this, &web_server](const std::string& endpoint,
                                std::function<void(const httplib::Request&, httplib::Response&)> handler) {
        web_server.Post("/api/v0/" + endpoint, handler);
        web_server.Post("/api/v1/" + endpoint, handler);
        web_server.Post("/v0/" + endpoint, handler);
        web_server.Post("/v1/" + endpoint, handler);
        if (endpoint != "params") {
            web_server.Get("/api/v0/" + endpoint, [](const httplib::Request&, httplib::Response& res) {
                res.status = 405;
                res.set_content("{\"error\": \"Method Not Allowed. Use POST for this endpoint\"}", "application/json");
            });
            web_server.Get("/api/v1/" + endpoint, [](const httplib::Request&, httplib::Response& res) {
                res.status = 405;
                res.set_content("{\"error\": \"Method Not Allowed. Use POST for this endpoint\"}", "application/json");
            });
            web_server.Get("/v0/" + endpoint, [](const httplib::Request&, httplib::Response& res) {
                res.status = 405;
                res.set_content("{\"error\": \"Method Not Allowed. Use POST for this endpoint\"}", "application/json");
            });
            web_server.Get("/v1/" + endpoint, [](const httplib::Request&, httplib::Response& res) {
                res.status = 405;
                res.set_content("{\"error\": \"Method Not Allowed. Use POST for this endpoint\"}", "application/json");
            });
        }
    };

    // Health check
    register_get("health", [this](const httplib::Request& req, httplib::Response& res) {
        handle_health(req, res);
    });

    // Models endpoints
    register_get("models", [this](const httplib::Request& req, httplib::Response& res) {
        handle_models(req, res);
    });

    // Explicit network action for users who disable startup update checks.
    register_post("models/check-updates", [this](const httplib::Request& req, httplib::Response& res) {
        handle_model_update_check(req, res);
    });

    // Model files endpoint for the Files tab. Register before the generic
    // /models/(.+) route so '<model-id>/files' is not parsed as the model ID.
    web_server.Get(R"(/api/v0/models/(.+)/files)", [this](const httplib::Request& req, httplib::Response& res) {
        handle_model_files(req, res);
    });
    web_server.Get(R"(/api/v1/models/(.+)/files)", [this](const httplib::Request& req, httplib::Response& res) {
        handle_model_files(req, res);
    });
    web_server.Get(R"(/v0/models/(.+)/files)", [this](const httplib::Request& req, httplib::Response& res) {
        handle_model_files(req, res);
    });
    web_server.Get(R"(/v1/models/(.+)/files)", [this](const httplib::Request& req, httplib::Response& res) {
        handle_model_files(req, res);
    });

    // Model by ID (need to register for both versions with regex, with and without /api prefix)
    web_server.Get(R"(/api/v0/models/(.+))", [this](const httplib::Request& req, httplib::Response& res) {
        handle_model_by_id(req, res);
    });
    web_server.Get(R"(/api/v1/models/(.+))", [this](const httplib::Request& req, httplib::Response& res) {
        handle_model_by_id(req, res);
    });
    web_server.Get(R"(/v0/models/(.+))", [this](const httplib::Request& req, httplib::Response& res) {
        handle_model_by_id(req, res);
    });
    web_server.Get(R"(/v1/models/(.+))", [this](const httplib::Request& req, httplib::Response& res) {
        handle_model_by_id(req, res);
    });

    // Chat completions (OpenAI compatible)
    register_post("chat/completions", [this](const httplib::Request& req, httplib::Response& res) {
        handle_chat_completions(req, res);
    });

    // Completions
    register_post("completions", [this](const httplib::Request& req, httplib::Response& res) {
        handle_completions(req, res);
    });

    // Embeddings
    register_post("embeddings", [this](const httplib::Request& req, httplib::Response& res) {
        handle_embeddings(req, res);
    });

    // Reranking
    register_post("reranking", [this](const httplib::Request& req, httplib::Response& res) {
        handle_reranking(req, res);
    });

    // Slots (llama.cpp backend information)
    register_get("slots", [this](const httplib::Request& req, httplib::Response& res) {
        handle_slots(req, res);
    });

    // Slots action endpoints (need to register for both versions with regex, with and without /api prefix)
    web_server.Post(R"(/api/v0/slots/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
        handle_slots_by_id(req, res);
    });
    web_server.Post(R"(/api/v1/slots/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
        handle_slots_by_id(req, res);
    });
    web_server.Post(R"(/v0/slots/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
        handle_slots_by_id(req, res);
    });
    web_server.Post(R"(/v1/slots/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
        handle_slots_by_id(req, res);
    });

    // Tokenize endpoint (llama.cpp specific)
    register_post("tokenize", [this](const httplib::Request& req, httplib::Response& res) {
        handle_tokenize(req, res);
    });

    // Audio endpoints (OpenAI /v1/audio/* compatible)
    register_post("audio/transcriptions", [this](const httplib::Request& req, httplib::Response& res) {
        handle_audio_transcriptions(req, res);
    });

    // Speech
    register_post("audio/speech", [this](const httplib::Request& req, httplib::Response& res) {
        handle_audio_speech(req, res);
    });

    // Image endpoints (OpenAI /v1/images/* compatible)
    register_post("images/generations", [this](const httplib::Request& req, httplib::Response& res) {
        handle_image_generations(req, res);
    });
    register_post("images/edits", [this](const httplib::Request& req, httplib::Response& res) {
        handle_image_edits(req, res);
    });
    register_post("images/variations", [this](const httplib::Request& req, httplib::Response& res) {
        handle_image_variations(req, res);
    });
    register_post("images/upscale", [this](const httplib::Request& req, httplib::Response& res) {
        handle_image_upscale(req, res);
    });
    // Generative-audio endpoint: text -> audio clip (music, sound effects)
    register_post("audio/generations", [this](const httplib::Request& req, httplib::Response& res) {
        handle_audio_generations(req, res);
    });
    register_post("3d/generations", [this](const httplib::Request& req, httplib::Response& res) {
        handle_3d_generations(req, res);
    });
    // Responses endpoint
    register_post("responses", [this](const httplib::Request& req, httplib::Response& res) {
        handle_responses(req, res);
    });

    // Model management endpoints
    register_post("pull", [this](const httplib::Request& req, httplib::Response& res) {
        handle_pull(req, res);
    });

    register_get("pull/variants", [this](const httplib::Request& req, httplib::Response& res) {
        handle_pull_variants(req, res);
    });

    register_get("downloads", [this](const httplib::Request& req, httplib::Response& res) {
        handle_downloads(req, res);
    });

    register_post("downloads/control", [this](const httplib::Request& req, httplib::Response& res) {
        handle_download_control(req, res);
    });


    register_post("load", [this](const httplib::Request& req, httplib::Response& res) {
        handle_load(req, res);
    });

    register_post("unload", [this](const httplib::Request& req, httplib::Response& res) {
        handle_unload(req, res);
    });

    register_post("delete", [this](const httplib::Request& req, httplib::Response& res) {
        handle_delete(req, res);
    });

    register_post("params", [this](const httplib::Request& req, httplib::Response& res) {
        handle_params(req, res);
    });

    register_get("params", [this](const httplib::Request& req, httplib::Response& res) {
        handle_config_get(req, res);
    });

    // Backend management endpoints
    register_post("install", [this](const httplib::Request& req, httplib::Response& res) {
        handle_install(req, res);
    });

    register_post("install/dry-run", [this](const httplib::Request& req, httplib::Response& res) {
        handle_install_dry_run(req, res);
    });

    register_post("uninstall", [this](const httplib::Request& req, httplib::Response& res) {
        handle_uninstall(req, res);
    });

    // System endpoints
    register_get("stats", [this](const httplib::Request& req, httplib::Response& res) {
        handle_stats(req, res);
    });

    register_get("system-info", [this](const httplib::Request& req, httplib::Response& res) {
        handle_system_info(req, res);
    });

    register_get("system-stats", [this](const httplib::Request& req, httplib::Response& res) {
        handle_system_stats(req, res);
    });

    register_post("log-level", [this](const httplib::Request& req, httplib::Response& res) {
        handle_log_level(req, res);
    });


    // NOTE: /api/v1/halt endpoint removed - use SIGTERM signal instead (like Python server)
    // The stop command now sends termination signal directly to the process

    // Internal shutdown endpoint (not part of public API)
    web_server.Post("/internal/shutdown", [this](const httplib::Request& req, httplib::Response& res) {
        handle_shutdown(req, res);
    });

    web_server.Post("/internal/telemetry/flush", [](const httplib::Request& req, httplib::Response& res) {
        lemon::telemetry::flush();
        res.status = 200;
        res.set_content(nlohmann::json{{"status", "flushed"}}.dump(), "application/json");
    });

    web_server.Post("/internal/pin", [this](const httplib::Request& req, httplib::Response& res) {
        handle_pin(req, res);
    });

    // Unified config endpoints (not part of public API)
    web_server.Post("/internal/set", [this](const httplib::Request& req, httplib::Response& res) {
        handle_config_set(req, res);
    });
    web_server.Get("/internal/config", [this](const httplib::Request& req, httplib::Response& res) {
        handle_config_get(req, res);
    });
    web_server.Get("/internal/config/defaults", [this](const httplib::Request& req, httplib::Response& res) {
        handle_config_defaults_get(req, res);
    });
    web_server.Post("/internal/cleanup-cache", [this](const httplib::Request& req, httplib::Response& res) {
        handle_cleanup_cache(req, res);
    });
    web_server.Post("/internal/simulate-vram-pressure", [this](const httplib::Request& req, httplib::Response& res) {
        handle_simulate_vram_pressure(req, res);
    });

    // Cloud auth: register quad-prefix POST and a parameterized DELETE.
    //   POST /v1/cloud/auth        body: {provider, api_key}
    //   DELETE /v1/cloud/auth/{p}
    // The runtime key lives in process memory only; env var
    // LEMONADE_<PROVIDER>_API_KEY takes precedence (POST returns 409 if it
    // is set). Both endpoints respect LEMONADE_ADMIN_API_KEY when configured
    // via the standard authentication path applied to /v1/.
    register_post("cloud/auth", [this](const httplib::Request& req, httplib::Response& res) {
        handle_cloud_auth_set(req, res);
    });
    web_server.Delete(R"(/api/v0/cloud/auth/(.+))", [this](const httplib::Request& req, httplib::Response& res) {
        handle_cloud_auth_clear(req, res);
    });
    web_server.Delete(R"(/api/v1/cloud/auth/(.+))", [this](const httplib::Request& req, httplib::Response& res) {
        handle_cloud_auth_clear(req, res);
    });
    web_server.Delete(R"(/v0/cloud/auth/(.+))", [this](const httplib::Request& req, httplib::Response& res) {
        handle_cloud_auth_clear(req, res);
    });
    web_server.Delete(R"(/v1/cloud/auth/(.+))", [this](const httplib::Request& req, httplib::Response& res) {
        handle_cloud_auth_clear(req, res);
    });

    // Test endpoint to verify POST works
    web_server.Post("/api/v1/test", [](const httplib::Request& req, httplib::Response& res) {
        LOG(INFO, "Server") << "TEST POST endpoint hit!" << std::endl;
        res.set_content("{\"test\": \"ok\"}", "application/json");
    });

    // Register Ollama-compatible API routes
    auto ollama_api = std::make_shared<OllamaApi>(router_.get(), model_manager_.get());
    ollama_api->register_routes(web_server);

    // Register MCP gateway (POST /mcp). NOTE: /mcp is an INTENTIONAL EXCEPTION
    // to the quad-prefix invariant (AGENTS.md #1) — the MCP spec mandates a
    // single endpoint URL.
    auto mcp_server = std::make_shared<McpServer>(
        router_.get(),
        model_manager_.get(),
        [this](const std::string& m) { auto_load_model_if_needed(m); });
    mcp_server->register_routes(web_server);

    // Setup static file serving for web UI
    setup_static_files(web_server);
}

void Server::setup_static_files(httplib::Server &web_server) {
    // Determine static files directory (relative to executable)
    std::string static_dir = utils::get_resource_path("resources/static");

    // Create a reusable handler for serving index.html with template variable replacement
    auto serve_index_html = [this, static_dir](const httplib::Request&, httplib::Response& res) {
        std::string index_path = static_dir + "/index.html";
        std::ifstream file(index_path);

        if (!file.is_open()) {
            LOG(ERROR, "Server") << "Could not open index.html at: " << index_path << std::endl;
            res.status = 404;
            res.set_content("{\"error\": \"index.html not found\"}", "application/json");
            return;
        }

        // Read the entire file
        std::string html_template((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        file.close();

        // Get filtered models from model manager
        auto models_map = model_manager_->get_supported_models();

        // Convert map to JSON
        json filtered_models = json::object();
        for (const auto& [model_name, info] : models_map) {
            std::vector<std::string> public_components;
            public_components.reserve(info.components.size());
            for (const auto& component : info.components) {
                public_components.push_back(model_manager_->get_public_model_name(component));
            }
            filtered_models[model_name] = {
                {"model_name", model_name},
                {"checkpoint", info.checkpoint()},
                {"recipe", info.recipe},
                {"labels", info.labels},
                {"suggested", info.suggested},
                {"source", info.source.empty() ? info.registry_source : info.source},
                {"registry_source", info.registry_source},
                {"components", public_components},
                {"mmproj", info.mmproj()}
            };

            // Add size if available
            if (info.size > 0.0) {
                filtered_models[model_name]["size"] = info.size;
            }
        }

        // Create JavaScript snippets
        std::string server_models_js = "<script>window.SERVER_MODELS = " + filtered_models.dump() + ";</script>";

        // Get platform name
        std::string platform_name;
        #ifdef _WIN32
            platform_name = "Windows";
        #elif __APPLE__
            platform_name = "Darwin";
        #elif __linux__
            platform_name = "Linux";
        #else
            platform_name = "Unknown";
        #endif
        std::string platform_js = "<script>window.PLATFORM = '" + platform_name + "';</script>";

        // Replace template variables
        size_t pos;

        // Replace {{SERVER_PORT}}
        while ((pos = html_template.find("{{SERVER_PORT}}")) != std::string::npos) {
            html_template.replace(pos, 15, std::to_string(port_));
        }

        // Replace {{SERVER_MODELS_JS}}
        while ((pos = html_template.find("{{SERVER_MODELS_JS}}")) != std::string::npos) {
            html_template.replace(pos, 20, server_models_js);
        }

        // Replace {{PLATFORM_JS}}
        while ((pos = html_template.find("{{PLATFORM_JS}}")) != std::string::npos) {
            html_template.replace(pos, 15, platform_js);
        }

        // Set no-cache headers
        res.set_header("Cache-Control", "no-cache, no-store, must-revalidate");
        res.set_header("Pragma", "no-cache");
        res.set_header("Expires", "0");
        res.set_content(html_template, "text/html");
    };

    // Keep status page at /status endpoint
    web_server.Get("/status", serve_index_html);

    // Also serve index.html at /api/v1 for compatibility
    web_server.Get("/api/v1", serve_index_html);

    // Mount static files directory for status page assets (CSS, JS, images)
    if (!web_server.set_mount_point("/static", static_dir)) {
        LOG(WARNING, "Server") << "Could not mount static files from: " << static_dir << std::endl;
        LOG(WARNING, "Server") << "Status page assets will not be available" << std::endl;
    }

    // Web app UI endpoint - serve the React web app at root
    std::string web_app_dir = utils::get_resource_path("resources/web-app");

    // Check if web app directory exists
    if (fs::exists(web_app_dir) && fs::is_directory(web_app_dir)) {
        // Create a handler for serving web app index.html for SPA routing
        auto serve_web_app_html = [web_app_dir](const httplib::Request&, httplib::Response& res) {
            std::string index_path = web_app_dir + "/index.html";
            std::ifstream file(index_path);

            if (!file.is_open()) {
                res.status = 404;
                res.set_content("{\"error\": \"Web app not found\"}", "application/json");
                return;
            }

            std::string html((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            file.close();

            // Inject mock window.api for web compatibility with the shared Tauri app renderer
            std::string mock_api = R"(
<script>
// Mock window.api for web compatibility (the Tauri shim is skipped in pure-web mode)
window.api = {
    isWebApp: true,  // Explicit flag to indicate web mode
    platform: navigator.platform || 'web',
    minimizeWindow: () => {},
    maximizeWindow: () => {},
    closeWindow: () => {},
    openExternal: (url) => window.open(url, '_blank'),
    onMaximizeChange: () => {},
    updateMinWidth: () => {},
    zoomIn: () => document.body.style.zoom = (parseFloat(document.body.style.zoom || '1') + 0.1).toString(),
    zoomOut: () => document.body.style.zoom = (parseFloat(document.body.style.zoom || '1') - 0.1).toString(),
    getSettings: async () => {
        const saved = localStorage.getItem('lemonade-settings');
        if (saved) return JSON.parse(saved);
        // Return defaults matching DEFAULT_LAYOUT_SETTINGS from appSettings.ts
        return {
            layout: {
                isChatVisible: true,
                isModelManagerVisible: true,
                isCenterPanelVisible: true,
                isLogsVisible: false,
                modelManagerWidth: 280,
                chatWidth: 350,
                logsHeight: 200
            },
            theme: 'dark',
            apiUrl: window.location.origin,
            apiKey: { value: '' }
        };
    },
    saveSettings: async (settings) => {
        localStorage.setItem('lemonade-settings', JSON.stringify(settings));
        return settings;
    },
    onSettingsUpdated: () => {},
    getServerPort: () => parseInt(window.location.port) || 13305,
    onServerPortUpdated: () => {},
    getServerAPIKey: async () => {
        const settings = await window.api.getSettings();
        return settings.apiKey?.value || '';
    },
    restartApp: () => window.location.reload(),
    writeClipboard: async (text) => {
        if (navigator.clipboard) {
            try {
                await navigator.clipboard.writeText(text);
                return;
            } catch {
                // Ignore clipboard errors and fall back to legacy method
            }
        }
        const ta = document.createElement('textarea');
        ta.value = String(text);
        ta.setAttribute('readonly', '');
        ta.style.position = 'fixed';
        ta.style.left = '-9999px';
        document.body.appendChild(ta);
        try {
            ta.select();
            if (!document.execCommand('copy')) {
                throw new Error('Legacy clipboard copy failed');
            }
        } finally {
            document.body.removeChild(ta);
        }
    }
};
</script>
)";

            // Insert mock API before the closing </head> tag
            size_t head_end_pos = html.find("</head>");
            if (head_end_pos != std::string::npos) {
                html.insert(head_end_pos, mock_api);
            }

            // Set no-cache headers
            res.set_header("Cache-Control", "no-cache, no-store, must-revalidate");
            res.set_header("Pragma", "no-cache");
            res.set_header("Expires", "0");
            res.set_content(html, "text/html");
        };

        // Serve the web app's index.html at root and for SPA routes
        web_server.Get("/", serve_web_app_html);

        // Also serve at /web-app for backwards compatibility
        web_server.Get("/web-app/?", serve_web_app_html);

        // Serve all static assets from the web app directory (JS, CSS, fonts, assets, etc.)
        // Handle both root-level assets and /web-app/ prefixed paths for backwards compatibility
        auto serve_web_app_asset = [web_app_dir](const httplib::Request& req, httplib::Response& res, const std::string& file_path) {
            std::error_code ec;
            namespace fs = std::filesystem;

            auto base = fs::weakly_canonical(fs::path(web_app_dir), ec);
            if (ec) {
                res.status = 500;
                res.set_content("Internal server error", "text/plain");
                return;
            }

            auto candidate = fs::weakly_canonical(base / file_path, ec);
            if (ec) {
                // Path doesn't exist or cannot be canonicalized
                res.status = 404;
                res.set_content("File not found", "text/plain");
                return;
            }

            // Verify the resolved path is confined under the base directory.
            // Use std::filesystem::relative (not string prefix) so it works
            // correctly on Windows where path separators differ.
            auto relative = fs::relative(candidate, base, ec);
            if (ec || relative.empty() || relative.is_absolute()) {
                // empty = candidate == base (directory, not a file)
                // is_absolute = somehow escaped (shouldn't happen after canonicalization)
                res.status = 403;
                res.set_content("Forbidden", "text/plain");
                return;
            }
            // Belt-and-suspenders: reject if any path component is ".."
            // (weakly_canonical should have resolved it, but this catches
            // edge cases on exotic filesystems). Check components, not
            // substrings, so legitimate filenames like "my..file.js" are allowed.
            for (const auto& part : relative) {
                if (part == "..") {
                    res.status = 403;
                    res.set_content("Forbidden", "text/plain");
                    return;
                }
            }

            // Serve the file
            std::ifstream file(candidate, std::ios::binary);
            if (!file.is_open()) {
                res.status = 404;
                res.set_content("File not found", "text/plain");
                return;
            }

            // Read file content
            std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            file.close();

            // Determine content type based on extension
            std::string content_type = "application/octet-stream";
            size_t dot_pos = file_path.rfind('.');
            if (dot_pos != std::string::npos) {
                std::string ext = file_path.substr(dot_pos);
                if (ext == ".js") content_type = "text/javascript";
                else if (ext == ".css") content_type = "text/css";
                else if (ext == ".html") content_type = "text/html";
                else if (ext == ".woff") content_type = "font/woff";
                else if (ext == ".woff2") content_type = "font/woff2";
                else if (ext == ".ttf") content_type = "font/ttf";
                else if (ext == ".svg") content_type = "image/svg+xml";
                else if (ext == ".png") content_type = "image/png";
                else if (ext == ".jpg" || ext == ".jpeg") content_type = "image/jpeg";
                else if (ext == ".json") content_type = "application/json";
                else if (ext == ".ico") content_type = "image/x-icon";
            }

            res.set_content(content, content_type);
        };

        // Serve favicon from web-app directory at root
        web_server.Get("/favicon.ico", [serve_web_app_asset](const httplib::Request& req, httplib::Response& res) {
            serve_web_app_asset(req, res, "favicon.ico");
        });

        // Serve web app assets from root (for files like renderer.bundle.js, fonts, etc.)
        web_server.Get(R"(/([^/]+\.(js|css|woff|woff2|ttf|svg|png|jpg|jpeg|json|ico)))",
                      [serve_web_app_asset](const httplib::Request& req, httplib::Response& res) {
            std::string file_path = req.matches[1].str();
            serve_web_app_asset(req, res, file_path);
        });

        // Keep /web-app/ prefix routes for backwards compatibility
        web_server.Get(R"(/web-app/(.+))", [serve_web_app_asset](const httplib::Request& req, httplib::Response& res) {
            std::string file_path = req.matches[1].str();
            serve_web_app_asset(req, res, file_path);
        });

        // SPA fallback: serve index.html for any unmatched GET routes that don't start with /api, /v0, /v1, /static, or /live
        // This enables client-side routing
        web_server.Get(R"(^(?!/api|/v0|/v1|/static|/live|/status|/internal).*)",
                      [serve_web_app_html](const httplib::Request& req, httplib::Response& res) {
            // Only serve index.html if the path doesn't look like a file with extension
            std::string path = req.path;
            size_t last_slash = path.rfind('/');
            std::string last_segment = (last_slash != std::string::npos) ? path.substr(last_slash + 1) : path;

            // If the last segment has an extension and it's not .html, let it 404
            // (This helps catch missing assets more clearly)
            size_t dot_pos = last_segment.rfind('.');
            if (dot_pos != std::string::npos) {
                std::string ext = last_segment.substr(dot_pos);
                if (ext != ".html" && ext != ".htm") {
                    // File with extension not found, return 404
                    res.status = 404;
                    return;
                }
            }

            // Otherwise, serve the SPA index.html for client-side routing
            serve_web_app_html(req, res);
        });
    } else {
        // Fallback to static page when web-app is not compiled
        LOG(INFO, "Server") << "Web app directory not found at: " << web_app_dir << std::endl;
        LOG(INFO, "Server") << "Falling back to static status page at root" << std::endl;

        // Serve the static status page at root instead
        web_server.Get("/", serve_index_html);

        // Serve favicon from static directory
        web_server.Get("/favicon.ico", [static_dir](const httplib::Request& req, httplib::Response& res) {
            std::ifstream ifs(static_dir + "/favicon.ico", std::ios::binary);
            if (ifs) {
                std::string content((std::istreambuf_iterator<char>(ifs)), (std::istreambuf_iterator<char>()));
                res.set_content(content, "image/x-icon");
                res.status = 200;
            } else {
                res.set_content("Favicon not found.", "text/plain");
                res.status = 404;
            }
        });
    }

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
        {"Access-Control-Allow-Origin", "*"},
        {"Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS"},
        {"Access-Control-Allow-Headers", "Content-Type, Authorization, X-Client-Session-Id, X-Account-Session-Id"}
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

        std::atomic<bool> listener_started(false);
        std::atomic<bool> listener_start_failed(false);

        if (!ipv4.empty()) {
            // setup ipv4 thread
            setup_http_logger(*http_server_);
            http_v4_thread_ = std::thread([this, ipv4, &listener_started, &listener_start_failed]() {
                LOG(INFO, "Server") << "Binding IPv4 HTTP server to " << ipv4 << ":" << port_ << "..." << std::endl;
                int result = http_front_->bind_to_port(ipv4, port_);
                if (result <= 0) {
                    LOG(ERROR, "Server") << "Failed to bind IPv4 HTTP server to " << ipv4 << ":" << port_ << std::endl;
                    listener_start_failed = true;
                    return;
                }
                // The routed server's keep-alive loop runs only while it sees
                // a valid listen socket
                http_server_->set_listen_socket(http_front_->listen_socket());
                LOG(INFO, "Server") << "IPv4 HTTP server listening on " << ipv4 << ":" << port_ << std::endl;
                listener_started = true;
                if (!http_front_->listen_after_bind()) {
                    LOG(ERROR, "Server") << "IPv4 HTTP server listen_after_bind() failed" << std::endl;
                    listener_start_failed = true;
                }
            });
        }
        if (!ipv6.empty()) {
            // setup ipv6 thread
            setup_http_logger(*http_server_v6_);
            http_v6_thread_ = std::thread([this, ipv6, &listener_started, &listener_start_failed]() {
                LOG(INFO, "Server") << "Binding IPv6 HTTP server to [" << ipv6 << "]:" << port_ << "..." << std::endl;
                int result = http_front_v6_->bind_to_port(ipv6, port_);
                if (result <= 0) {
                    LOG(ERROR, "Server") << "Failed to bind IPv6 HTTP server to [" << ipv6 << "]:" << port_ << std::endl;
                    listener_start_failed = true;
                    return;
                }
                http_server_v6_->set_listen_socket(http_front_v6_->listen_socket());
                LOG(INFO, "Server") << "IPv6 HTTP server listening on [" << ipv6 << "]:" << port_ << std::endl;
                listener_started = true;
                if (!http_front_v6_->listen_after_bind()) {
                    LOG(ERROR, "Server") << "IPv6 HTTP server listen_after_bind() failed" << std::endl;
                    listener_start_failed = true;
                }
            });
        }

        // Enumerate all RFC1918 interfaces to determine if we can broadcast.
        // The beacon will send per-interface with the correct IP in the payload.
        auto rfc1918Interfaces = udp_beacon_.getLocalRFC1918Interfaces();
        bool no_bcast = config_->no_broadcast();
        if (!rfc1918Interfaces.empty() && !no_bcast) {
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
        } else if (!rfc1918Interfaces.empty() && no_bcast) {
            LOG(INFO, "Server") << "Broadcasting disabled by --no-broadcast option" << std::endl;
        } else {
            LOG(INFO, "Server") << "Unable to broadcast my existance please use a RFC1918 IPv4," << std::endl
                        << "or hostname that resolves to RFC1918 IPv4." << std::endl;
        }

        // Wait for listener threads, but check periodically for shutdown or rebind signals.
        // The threads are blocked in listen_after_bind(), which only returns when
        // the server is stopped or an error occurs.
        while ((http_v4_thread_.joinable() || http_v6_thread_.joinable()) &&
               !shutdown_requested_.load() && !rebind_requested_.load()) {
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

        if (!listener_started && listener_start_failed) {
            if (rebind_requested_) {
                // Port rebind failed (e.g. port in use) — restore old port and retry
                LOG(ERROR, "Server") << "Failed to bind to new port " << port_
                            << ", will not retry" << std::endl;
                rebind_requested_ = false;
                break;
            }
            std::cerr << "[Server] Another Lemonade router/server instance is already running on "
                      << config_->host() << ":" << port_ << ". Duplicate instance now exiting." << std::endl;
            stop();
            break;
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

        // Stop WebSocket server
        if (websocket_server_) {
            LOG(INFO, "Server") << "Stopping WebSocket server..." << std::endl;
            websocket_server_->stop();
        }

        // Explicitly clean up router (unload models, stop backend servers)
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

    if (model_cache_warmup_thread_.joinable()) {
        model_cache_warmup_thread_.join();
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

    // Case 3: Model exists and is available, but failed to load (engine error or pinned slots constraint)
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
    const json& request_options) {
    // Check if this specific model is already loaded (multi-model aware)
    if (router_->is_model_loaded(requested_model)) {
        LOG(DEBUG, "Server") << "Model already loaded: " << requested_model << std::endl;
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
    router_->load_model(requested_model, info, RecipeOptions(info.recipe, request_options), true);
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
        // Per the documented contract, per-model options like ctx_size or
        // llamacpp_backend are NOT forwarded from the collection's load request
        // to its components. Each component uses its own saved recipe_options.json
        // entry.
        router_->load_model(component, comp_info, comp_info.recipe_options, true,
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
        auto updated_models = model_manager_->check_for_model_updates();
        nlohmann::json response = {
            {"status", "success"},
            {"updates_available", updated_models.size()},
            {"models", updated_models}
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

    res.set_content(response.dump(), "application/json");
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
        {"source", info.source.empty() ? info.registry_source : info.source},
        {"registry_source", info.registry_source},
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
    std::string model_id = req.matches[1];

    if (model_manager_->model_exists(model_id)) {
        auto info = model_manager_->get_model_info(model_id);
        // Emit the wire-format id (bare for the precedence-winner, canonical-prefixed
        // for shadowed sources), regardless of which form the client requested.
        std::string canonical_cache_key = model_manager_->resolve_model_name(model_id);
        std::string wire_id = model_manager_->get_public_model_name(canonical_cache_key);
        res.set_content(model_info_to_json(wire_id, info).dump(), "application/json");
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
        *router_, [this](const std::string& m) { auto_load_model_if_needed(m); });
    RoutingPolicyEngine engine(std::move(policy), std::move(services));

    RouteContext ctx = build_route_context(request_json, collection_info.model_name);
    const bool want_trace = request_json.value("route_trace", false);
    Decision decision = engine.route(ctx, want_trace);
    RouterDispatchResult result;
    result.requested_model = collection_info.model_name;
    result.selected_model = decision.route_to;
    result.decision = std::move(decision);
    return result;
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
    } catch (const std::exception& e) {
        LOG(WARNING, "Server") << "Router collection dispatch failed for '"
                               << requested_model << "': " << e.what() << std::endl;
    }
    return std::nullopt;
}

void Server::handle_chat_completions(const httplib::Request& req, httplib::Response& res) {
    nlohmann::json request_json;
    if (!parse_required_json_body(req, res, request_json)) return;

    try {

        // Normalize client-provided model names (e.g., strip ":latest" suffix)
        // Must be done before any model_manager/router lookups and before forwarding
        normalize_client_model_name(request_json);

        // Debug: Check if tools are present
        if (request_json.contains("tools")) {
            LOG(DEBUG, "Server") << "Tools present in request: " << request_json["tools"].size() << " tool(s)" << std::endl;
            LOG(DEBUG, "Server") << "Tools JSON: " << request_json["tools"].dump() << std::endl;
        } else {
            LOG(DEBUG, "Server") << "No tools in request" << std::endl;
        }

        bool request_modified = false;
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

        // Use original request body - each backend (FLM, llamacpp, etc.) handles
        // model name transformation internally via their forward methods.
        // Note: request_json was already normalized at the top of this function
        std::string request_body = req.body;

        // OpenCode and other OpenAI-compatible clients may send thinking=false
        // instead of Lemonade's enable_thinking=false.
        if (should_disable_thinking(request_json)) {
            request_modified = prepend_no_think_to_last_user_message(request_json) || request_modified;
        }
        request_modified = strip_handled_thinking_fields(request_json) || request_modified;

        // If we modified the request (or normalized the model name earlier), serialize to string
        // The early normalize_client_model_name() call modifies request_json but doesn't set a flag,
        // so we always use request_json for the body to ensure model name normalization is applied
        request_body = request_json.dump();

        if (is_streaming) {
            try {
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

            // Print and save telemetry for non-streaming
            // llama-server includes timing data in the response under "timings" field
            if (response.contains("timings")) {
                auto timings = response["timings"];
                int input_tokens = 0;
                int output_tokens = 0;
                double ttft_seconds = 0.0;
                double tps = 0.0;

                if (timings.contains("prompt_n")) {
                    input_tokens = timings["prompt_n"].get<int>();
                }
                if (timings.contains("predicted_n")) {
                    output_tokens = timings["predicted_n"].get<int>();
                }
                if (timings.contains("prompt_ms")) {
                    ttft_seconds = timings["prompt_ms"].get<double>() / 1000.0;
                }
                if (timings.contains("predicted_per_second")) {
                    tps = timings["predicted_per_second"].get<double>();
                }

                std::string model_name = request_json.value("model", "");
                LOG(INFO, "Telemetry") << "Inference completed: model=" << model_name
                                       << ", tokens=" << (input_tokens + output_tokens)
                                       << " (in=" << input_tokens << ", out=" << output_tokens << ")"
                                       << ", ttft=" << std::fixed << std::setprecision(2) << ttft_seconds << "s"
                                       << ", tps=" << tps << std::endl;

                LOG(DEBUG, "Telemetry") << "=== Telemetry ===\n"
                                        << "Model:         " << model_name << "\n"
                                        << "Input tokens:  " << input_tokens << "\n"
                                        << "Output tokens: " << output_tokens << "\n"
                                        << "TTFT (s):      " << std::fixed << std::setprecision(2) << ttft_seconds << "\n"
                                        << "TPS:           " << std::fixed << std::setprecision(2) << tps << "\n"
                                        << "=================" << std::endl;

                // Save telemetry to router
                router_->update_telemetry(model_name, input_tokens, output_tokens,
                                          ttft_seconds, tps);
            } else if (response.contains("usage")) {
                // OpenAI format uses "usage" field
                auto usage = response["usage"];
                int input_tokens = 0;
                int output_tokens = 0;
                double ttft_seconds = 0.0;
                double tps = 0.0;

                if (usage.contains("prompt_tokens")) {
                    input_tokens = usage["prompt_tokens"].get<int>();
                }
                if (usage.contains("completion_tokens")) {
                    output_tokens = usage["completion_tokens"].get<int>();
                }

                // FLM format may include timing data
                if (usage.contains("prefill_duration_ttft")) {
                    ttft_seconds = usage["prefill_duration_ttft"].get<double>();
                }
                if (usage.contains("decoding_speed_tps")) {
                    tps = usage["decoding_speed_tps"].get<double>();
                }

                std::string model_name = request_json.value("model", "");
                LOG(INFO, "Telemetry") << "Inference completed: model=" << model_name
                                       << ", tokens=" << (input_tokens + output_tokens)
                                       << " (in=" << input_tokens << ", out=" << output_tokens << ")"
                                       << ", ttft=" << std::fixed << std::setprecision(2) << ttft_seconds << "s"
                                       << ", tps=" << tps << std::endl;

                LOG(DEBUG, "Telemetry") << "=== Telemetry ===\n"
                                        << "Model:         " << model_name << "\n"
                                        << "Input tokens:  " << input_tokens << "\n"
                                        << "Output tokens: " << output_tokens << "\n"
                                        << "TTFT (s):      " << std::fixed << std::setprecision(2) << ttft_seconds << "\n"
                                        << "TPS:           " << std::fixed << std::setprecision(2) << tps << "\n"
                                        << "=================" << std::endl;

                // Save telemetry to router
                router_->update_telemetry(model_name, input_tokens, output_tokens,
                                          ttft_seconds, tps);
            }

            // Capture prompt_tokens from usage if available
            if (response.contains("usage")) {
                auto usage = response["usage"];
                if (usage.contains("prompt_tokens")) {
                    int prompt_tokens = usage["prompt_tokens"].get<int>();
                    router_->update_prompt_tokens(request_json.value("model", ""), prompt_tokens);
                }
            }


        }

    } catch (const std::exception& e) {
        LOG(ERROR, "Server") << "Chat completion failed: " << e.what() << std::endl;

        res.status = 500;
        nlohmann::json error = {{"error", e.what()}};
        res.set_content(error.dump(), "application/json");
    }
}

void Server::handle_completions(const httplib::Request& req, httplib::Response& res) {
    try {
        auto request_json = nlohmann::json::parse(req.body);

        // Normalize client-provided model names (e.g., strip ":latest" suffix)
        // Must be done before any model_manager/router lookups and before forwarding
        normalize_client_model_name(request_json);

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

        // Use normalized request - model name was already normalized at the top
        std::string request_body = request_json.dump();

        if (is_streaming) {
            try {
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

            // Print and save telemetry for non-streaming completions
            if (response.contains("timings")) {
                auto timings = response["timings"];
                int input_tokens = 0;
                int output_tokens = 0;
                double ttft_seconds = 0.0;
                double tps = 0.0;

                if (timings.contains("prompt_n")) {
                    input_tokens = timings["prompt_n"].get<int>();
                }
                if (timings.contains("predicted_n")) {
                    output_tokens = timings["predicted_n"].get<int>();
                }
                if (timings.contains("prompt_ms")) {
                    ttft_seconds = timings["prompt_ms"].get<double>() / 1000.0;
                }
                if (timings.contains("predicted_per_second")) {
                    tps = timings["predicted_per_second"].get<double>();
                }

                std::string model_name = request_json.value("model", "");
                LOG(INFO, "Telemetry") << "Inference completed: model=" << model_name
                                       << ", tokens=" << (input_tokens + output_tokens)
                                       << " (in=" << input_tokens << ", out=" << output_tokens << ")"
                                       << ", ttft=" << std::fixed << std::setprecision(2) << ttft_seconds << "s"
                                       << ", tps=" << tps << std::endl;

                LOG(DEBUG, "Telemetry") << "=== Telemetry ===\n"
                                        << "Model:         " << model_name << "\n"
                                        << "Input tokens:  " << input_tokens << "\n"
                                        << "Output tokens: " << output_tokens << "\n"
                                        << "TTFT (s):      " << std::fixed << std::setprecision(2) << ttft_seconds << "\n"
                                        << "TPS:           " << std::fixed << std::setprecision(2) << tps << "\n"
                                        << "=================" << std::endl;

                // Save telemetry to router
                router_->update_telemetry(model_name, input_tokens, output_tokens,
                                          ttft_seconds, tps);
            } else if (response.contains("usage")) {
                auto usage = response["usage"];
                int input_tokens = 0;
                int output_tokens = 0;
                double ttft_seconds = 0.0;
                double tps = 0.0;

                if (usage.contains("prompt_tokens")) {
                    input_tokens = usage["prompt_tokens"].get<int>();
                }
                if (usage.contains("completion_tokens")) {
                    output_tokens = usage["completion_tokens"].get<int>();
                }

                // FLM format may include timing data
                if (usage.contains("prefill_duration_ttft")) {
                    ttft_seconds = usage["prefill_duration_ttft"].get<double>();
                }
                if (usage.contains("decoding_speed_tps")) {
                    tps = usage["decoding_speed_tps"].get<double>();
                }

                std::string model_name = request_json.value("model", "");
                LOG(INFO, "Telemetry") << "Inference completed: model=" << model_name
                                       << ", tokens=" << (input_tokens + output_tokens)
                                       << " (in=" << input_tokens << ", out=" << output_tokens << ")"
                                       << ", ttft=" << std::fixed << std::setprecision(2) << ttft_seconds << "s"
                                       << ", tps=" << tps << std::endl;

                LOG(DEBUG, "Telemetry") << "=== Telemetry ===\n"
                                        << "Model:         " << model_name << "\n"
                                        << "Input tokens:  " << input_tokens << "\n"
                                        << "Output tokens: " << output_tokens << "\n"
                                        << "TTFT (s):      " << std::fixed << std::setprecision(2) << ttft_seconds << "\n"
                                        << "TPS:           " << std::fixed << std::setprecision(2) << tps << "\n"
                                        << "=================" << std::endl;

                // Save telemetry to router
                router_->update_telemetry(model_name, input_tokens, output_tokens,
                                          ttft_seconds, tps);
            }

            // Capture prompt_tokens from usage if available
            if (response.contains("usage")) {
                auto usage = response["usage"];
                if (usage.contains("prompt_tokens")) {
                    int prompt_tokens = usage["prompt_tokens"].get<int>();
                    router_->update_prompt_tokens(request_json.value("model", ""), prompt_tokens);
                }
            }
        }

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
        res.set_content(response.dump(), "application/json");

    } catch (const std::exception& e) {
        LOG(ERROR, "Server") << "ERROR in handle_reranking: " << e.what() << std::endl;
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
        nlohmann::json request_json;

        // Extract form fields
        if (req.form.has_field("model")) {
            request_json["model"] = req.form.get_field("model");
        }
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

        // Check for error in response
        if (response.contains("error")) {
            set_error_response(response, res, 500);
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

        // Handle model loading
        if (request_json.contains("model")) {
            std::string requested_model = request_json["model"];
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

        bool is_streaming = (request_json.contains("stream") && request_json["stream"].get<bool>());

        if (request_json.contains("stream_format")) {
            is_streaming = true;
            if (request_json["stream_format"] != "audio") {
                res.status = 400;
                nlohmann::json error = {{"error", {
                    {"message", "Only pcm audio streaming format is supported"},
                    {"type", "invalid_request_error"}
                }}};
                res.set_content(error.dump(), "application/json");
                return;
            }
        }

        const auto supported_formats =
            router_->audio_speech_supported_formats(request_json["model"].get<std::string>());
        std::string response_format = "mp3";
        if (is_streaming) {
            response_format = "pcm";
        } else if (request_json.contains("response_format") && request_json["response_format"].is_string()) {
            response_format = request_json["response_format"].get<std::string>();
        } else if (!supported_formats.empty()) {
            response_format = supported_formats.front();
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
        std::string mime_type = MIME_TYPES[response_format];

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
        res.status = 500;
        const auto& err = error_payload["error"];
        if (err.is_object() && err.contains("status") && err["status"].is_number_integer()) {
            const int status = err["status"].get<int>();
            if (status >= 400 && status <= 599) {
                res.status = status;
            }
        }
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
        for (const auto* field : {"lyrics", "vocal_language"}) {
            if (request_json.contains(field) && !request_json[field].is_string()) {
                res.status = 400;
                res.set_content(nlohmann::json{{"error", {
                    {"message", "'" + std::string(field) + "' must be a string"},
                    {"type", "invalid_request_error"}}}}.dump(), "application/json");
                return;
            }
        }

        std::string requested_model = request_json["model"];
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
        std::string mime_type = MIME_TYPES.contains(response_format)
            ? MIME_TYPES[response_format] : MIME_TYPES["wav"];

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

bool Server::extract_image_from_form(const httplib::Request& req, httplib::Response& res, nlohmann::json& out) {
    for (const auto& file_pair : req.form.files) {
        if (file_pair.first == "image" || file_pair.first == "image[]") {
            const auto& file = file_pair.second;
            out["image_data"] = utils::JsonUtils::base64_encode(file.content);
            out["image_filename"] = file.filename;
            LOG(INFO, "Server") << "Image file: " << file.filename
                      << " (" << file.content.size() << " bytes)" << std::endl;
            return true;
        }
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
        if (!extract_image_from_form(req, res, request_json)) return;

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

        std::string upscale_model_path;
        std::string backend;
        try {
            auto info = model_manager_->get_model_info(upscale_model_name);

            if (!model_manager_->is_model_downloaded(upscale_model_name)) {
                LOG(INFO, "Server") << "Upscale model not cached, downloading from its remote registry..." << std::endl;
                model_manager_->download_registered_model(info, true);
                LOG(INFO, "Server") << "Upscale model download complete: " << upscale_model_name << std::endl;
                info = model_manager_->get_model_info(upscale_model_name);
            }

            upscale_model_path = info.resolved_path("main");

            // Honor explicit config first (e.g. sdcpp.backend = "rocm").
            // "auto" in config.json is mapped to "" by recipe_options().
            auto recipe_opts = config_->recipe_options("");
            if (recipe_opts.contains("sd-cpp_backend") &&
                recipe_opts["sd-cpp_backend"].is_string()) {
                backend = recipe_opts["sd-cpp_backend"].get<std::string>();
            }

            // Auto-detect best backend when not explicitly configured,
            // matching the same logic SDServer::load() uses via
            // RecipeOptions::get_option(). Without this, upscaling
            // silently falls back to CPU even when ROCm/Vulkan is available.
            if (backend.empty()) {
                auto supported = SystemInfo::get_supported_backends("sd-cpp");
                if (!supported.backends.empty()) {
                    backend = supported.backends[0];
                } else {
                    backend = "cpu";
                }
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

        // sd-server's HTTP API does not expose an upscaling endpoint.
        // Upscaling is only available via the sd-cli binary's -M upscale mode,
        // so we shell out to sd-cli as a subprocess. This also keeps upscaling
        // as a separate request from generation, which lets the frontend show
        // the original and upscaled images side by side with independent timing.
        std::string exe_dir = lemon::backends::BackendUtils::get_backend_binary_path(
            *lemon::backends::try_get_spec_for_recipe("sd-cpp"), backend);
        std::filesystem::path cli_exe = std::filesystem::path(exe_dir).parent_path() /
#ifdef _WIN32
            "sd-cli.exe";
#else
            "sd-cli";
#endif

        if (!std::filesystem::exists(cli_exe)) {
            res.status = 500;
            nlohmann::json error = {{"error", {
                {"message", "sd-cpp backend not installed (sd-cli not found at: "
                            + cli_exe.string() + ")"},
                {"type", "server_error"}
            }}};
            res.set_content(error.dump(), "application/json");
            return;
        }

        std::vector<std::pair<std::string, std::string>> env_vars;
        std::filesystem::path cli_dir = cli_exe.parent_path();

        std::string resolved_backend = backend;
        if (backend == "rocm") {
            std::string channel = "stable";
            if (config_) {
                channel = config_->rocm_channel_for_recipe("sd-cpp");
            }
            resolved_backend = "rocm-" + channel;
        }
#ifndef _WIN32
        std::string lib_path = cli_dir.string();

        if (resolved_backend == "rocm-stable") {
            std::string rocm_arch = SystemInfo::get_rocm_arch();
            if (!rocm_arch.empty()) {
                std::string therock_lib = lemon::backends::BackendUtils::get_therock_lib_path(rocm_arch);
                if (!therock_lib.empty()) {
                    lib_path = therock_lib + ":" + lib_path;
                }
            }
        }

        const char* existing_ld_path = std::getenv("LD_LIBRARY_PATH");
        if (existing_ld_path && strlen(existing_ld_path) > 0) {
            lib_path = lib_path + ":" + std::string(existing_ld_path);
        }
        env_vars.push_back({"LD_LIBRARY_PATH", lib_path});
#else
        if (resolved_backend == "rocm-stable") {
            std::string new_path = cli_dir.string();
            std::string rocm_arch = SystemInfo::get_rocm_arch();
            if (!rocm_arch.empty()) {
                std::string therock_bin = lemon::backends::BackendUtils::get_therock_lib_path(rocm_arch);
                if (!therock_bin.empty()) {
                    new_path = therock_bin + ";" + new_path;
                }
            }

            const char* existing_path = std::getenv("PATH");
            if (existing_path && strlen(existing_path) > 0) new_path += ";" + std::string(existing_path);
            env_vars.push_back({"PATH", new_path});
        }
#endif

        std::string b64_image = request_json["image"].get<std::string>();
        std::string upscaled = lemon::backends::SDServer::upscale_via_cli(
            b64_image, upscale_model_path, cli_exe.string(), env_vars);

        if (upscaled.empty()) {
            res.status = 500;
            nlohmann::json error = {{"error", {
                {"message", "ESRGAN upscale failed"},
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
    try {
        auto request_json = nlohmann::json::parse(req.body);

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

        // Re-serialize so any collection.router model rewrite reaches the backend
        // (streaming forwards this body verbatim).
        std::string request_body = request_json.dump();

        if (is_streaming) {
            try {
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
        }

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
    } catch (const nlohmann::json::parse_error& e) {
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

        // Validate and canonicalize remote-registry provenance before anything is
        // persisted. `source` remains backward-compatible with local origins, while
        // remote registrations store a canonical huggingface/modelscope value.
        if (request_json.contains("source") || request_json.contains("registry_source")) {
            try {
                std::optional<std::string> normalized_registry;
                if (request_json.contains("registry_source")) {
                    normalized_registry = remote_registry_source_name(
                        parse_remote_registry_source(
                            request_json["registry_source"].get<std::string>()));
                }

                if (request_json.contains("source")) {
                    const std::string public_source = request_json["source"].get<std::string>();
                    if (is_remote_registry_source(public_source)) {
                        const std::string normalized_public = remote_registry_source_name(
                            parse_remote_registry_source(public_source));
                        if (normalized_registry && *normalized_registry != normalized_public) {
                            bad_request("'source' and 'registry_source' must identify the same registry");
                            return;
                        }
                        normalized_registry = normalized_public;
                        request_json["source"] = normalized_public;
                    } else if (!local_import && public_source != "local_upload" &&
                               public_source != "local_path" &&
                               public_source != "extra_models_dir") {
                        bad_request("Unsupported model source '" + public_source +
                                    "' (expected 'huggingface' or 'modelscope')");
                        return;
                    }
                }

                if (normalized_registry) {
                    if (!request_json.contains("source") ||
                        is_remote_registry_source(request_json["source"].get<std::string>())) {
                        request_json["source"] = *normalized_registry;
                    }
                    request_json["registry_source"] = *normalized_registry;
                }
            } catch (const std::exception& e) {
                bad_request(e.what());
                return;
            }
        }

        LOG(INFO, "Server") << "Pulling model: " << model_name << std::endl;
        if (!checkpoint.empty()) {
            LOG(INFO, "Server") << "   checkpoint: " << checkpoint << std::endl;
        }
        if (!recipe.empty()) {
            LOG(INFO, "Server") << "   recipe: " << recipe << std::endl;
        }

        // Reject reserved prefixes — extra.* / builtin.* cannot be created via
        // /pull, and user.extra.* / user.builtin.* are also rejected because
        // their bare-name part ("extra.X" / "builtin.X") would otherwise hijack
        // the corresponding canonical alias slot. Then enforce the existing rule
        // that explicit checkpoint/recipe requires the user.* prefix.
        if (lemon::is_reserved_registration_name(model_name)) {
            res.status = 400;
            nlohmann::json error = {{"error",
                "Model names with 'extra.' / 'builtin.' prefixes are reserved, "
                "including as bare-name parts of a 'user.' alias. "
                "Use 'user.<name>' for registration where <name> does not begin "
                "with 'extra.' or 'builtin.'. Received: " + model_name}};
            res.set_content(error.dump(), "application/json");
            return;
        }
        if (!checkpoint.empty() || !recipe.empty()) {
            if (model_name.substr(0, 5) != "user.") {
                bad_request(
                    "When providing 'checkpoint' or 'recipe', the model name must include the "
                    "`user.` prefix, for example `user.Phi-4-Mini-GGUF`. Received: " + model_name);
                return;
            }
        }

        if (is_model_collection_recipe(recipe)) {
            if (auto err = model_manager_->validate_collection_request(model_name, request_json)) {
                bad_request(*err);
                return;
            }
            // A body carrying a `models` array is a collection file import: its
            // components may not be registered yet (download_model registers them
            // from the embedded definitions and canonicalizes the list itself).
            // A pointer body (registry-backed collection) has no `components` at all —
            // they come from the downloaded manifest. Only pre-canonicalize an
            // inline `components` list whose entries must already exist.
            if (request_json.contains("components") && request_json["components"].is_array() &&
                (!request_json.contains("models") || !request_json["models"].is_array())) {
                // Canonicalize components so downstream cache lookups
                // (check_component_downloaded, update_model_in_cache) succeed
                // even when the client passed a public alias (bare name) rather
                // than the canonical `user.X` / `builtin.X` form.
                for (auto& c : request_json["components"]) {
                    c = model_manager_->resolve_model_name(c.get<std::string>());
                }
            }
        }

        // Local import mode: CLI has already copied files to HF cache, just resolve and register
        if (local_import) {
            std::string hf_cache = model_manager_->get_hf_cache_dir();
            std::string model_name_clean = model_name.substr(5); // Remove "user." prefix
            std::replace(model_name_clean.begin(), model_name_clean.end(), '/', '-');
            std::string dest_path = hf_cache + "/models--" + model_name_clean;

            LOG(INFO, "Server") << "Local import mode - resolving files in: " << dest_path << std::endl;

            resolve_and_register_local_model(
                dest_path, model_name, request_json, hf_cache
            );

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

        if (stream) {
            auto operation = [this, model_name, request_json, do_not_upgrade](DownloadProgressCallback progress_cb) {
                model_manager_->download_model(model_name, request_json, do_not_upgrade, progress_cb);
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
            model_manager_->download_model(model_name, request_json, do_not_upgrade);

            nlohmann::json response = {{"status", "success"}, {"model_name", model_name}};
            res.set_content(response.dump(), "application/json");
        }

    } catch (const lemon::UnknownModelError& e) {
        LOG(ERROR, "Server") << "ERROR in handle_pull: " << e.what() << std::endl;
        res.status = 400;
        nlohmann::json error = {{"error", e.what()}, {"code", lemon::kUnknownModelErrorCode}};
        res.set_content(error.dump(), "application/json");
    } catch (const std::exception& e) {
        LOG(ERROR, "Server") << "ERROR in handle_pull: " << e.what() << std::endl;
        res.status = 500;
        nlohmann::json error = {{"error", e.what()}};
        res.set_content(error.dump(), "application/json");
    }
}

void Server::handle_pull_variants(const httplib::Request& req, httplib::Response& res) {
    try {
        std::string checkpoint = req.get_param_value("checkpoint");
        if (checkpoint.empty()) {
            res.status = 400;
            nlohmann::json error = {{"error", "Missing required query parameter 'checkpoint'"}};
            res.set_content(error.dump(), "application/json");
            return;
        }
        if (checkpoint.find('/') == std::string::npos) {
            res.status = 400;
            nlohmann::json error = {{"error",
                "Malformed 'checkpoint': expected a repository id of the form 'owner/name'"}};
            res.set_content(error.dump(), "application/json");
            return;
        }
        const std::string source = req.has_param("source")
            ? req.get_param_value("source") : "huggingface";
        const auto parsed_source = parse_remote_registry_source(source);
        if (config_->offline()) {
            res.status = 400;
            nlohmann::json error = {{"error", "Lemond is in offline mode, models not downloaded"}, {"code", "lemond_offline"}};
            res.set_content(error.dump(), "application/json");
            return;
        }
        bool not_found = false;
        nlohmann::json body = lemon::fetch_pull_variants(
            checkpoint, remote_registry_source_name(parsed_source), not_found);
        if (not_found) {
            res.status = 404;
            nlohmann::json error = {{"error", "Checkpoint '" + checkpoint + "' not found on " +
                remote_registry_display_name(parsed_source)}};
            res.set_content(error.dump(), "application/json");
            return;
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

        // Extract optional per-model settings (defaults to -1 / empty = use Router defaults)
        RecipeOptions options = RecipeOptions(info.recipe, request_json);
        bool save_options = request_json.value("save_options", false);
        std::optional<bool> pinned_opt = std::nullopt;
        if (request_json.contains("pinned") && request_json["pinned"].is_boolean()) {
            pinned_opt = request_json["pinned"].get<bool>();
        }

        LOG(INFO, "Server") << "Ensuring model loaded: " << model_name;
        LOG(INFO, "Server") << " " << options.to_log_string(false);
        LOG(INFO, "Server") << std::endl;

        // Persist request options to model info if requested
        if (save_options) {
            info.recipe_options = options;
            model_manager_->save_model_options(info);
        }

        // Download model if needed (first-time use or missing files). Collections have no
        // checkpoint of their own, so skip the generic HF download path here
        // and let the per-component branch below cascade any missing pieces.
        if (!model_manager_->is_model_downloaded(model_name) && !is_model_collection_recipe(info.recipe)) {
            LOG(INFO, "Server") << "Model not downloaded, downloading..." << std::endl;
            model_manager_->download_registered_model(info);
            info = model_manager_->get_model_info(model_name);
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

void Server::persist_cloud_providers() {
    if (cache_dir_.empty() || !cloud_registry_) return;
    try {
        json snap = config_->snapshot();
        snap["cloud_providers"] = cloud_registry_->to_config_array();
        ConfigFile::save(cache_dir_, snap);
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
                cloud_registry_->install(provider, base_url, true);
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

void Server::handle_params(const httplib::Request& req, httplib::Response& res) {
    try {
        auto body = nlohmann::json::parse(req.body);

        // Delegate to RuntimeConfig — accepts all known recipe option keys
        auto result = config_->set(body, [this](const json& applied) {
            apply_config_side_effects(applied);
        });
        res.set_content(result.dump(), "application/json");
    } catch (const nlohmann::json::parse_error& e) {
        LOG(ERROR, "Server") << "ERROR in handle_params: invalid JSON: " << e.what() << std::endl;
        res.status = 400;
        nlohmann::json error = {{"error", "Invalid JSON in request body"}};
        res.set_content(error.dump(), "application/json");
    } catch (const std::invalid_argument& e) {
        res.status = 400;
        nlohmann::json error = {{"error", e.what()}};
        res.set_content(error.dump(), "application/json");
    } catch (const std::exception& e) {
        LOG(ERROR, "Server") << "ERROR in handle_params: " << e.what() << std::endl;
        res.status = 500;
        nlohmann::json error = {{"error", e.what()}};
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
        system_metrics.gpu_percent = get_gpu_usage();
        system_metrics.vram_gb = get_vram_usage();
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

// Get GPU usage percentage (AMD GPUs on Linux)
double Server::get_gpu_usage() {
    return metrics_platform_->get_gpu_usage();
}

// Get VRAM/GTT usage in GB (AMD GPUs on Linux)
double Server::get_vram_usage() {
    return metrics_platform_->get_vram_usage_gb();
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

    // GPU usage
    double gpu_percent = get_gpu_usage();
    stats["gpu_percent"] = (gpu_percent >= 0) ? nlohmann::json(gpu_percent) : nlohmann::json();

    // VRAM usage
    double vram_gb = get_vram_usage();
    stats["vram_gb"] = (vram_gb >= 0) ? nlohmann::json(vram_gb) : nlohmann::json();

    // NPU Utilization
    double npu_percent = get_npu_utilization();
    stats["npu_percent"] = (npu_percent >= 0) ? nlohmann::json(npu_percent) : nlohmann::json();

    res.set_content(stats.dump(), "application/json");
}

void Server::handle_log_level(const httplib::Request& req, httplib::Response& res) {
    try {
        auto request_json = nlohmann::json::parse(req.body);

        // Translate {"level":"debug"} -> config_->set({"log_level":"debug"})
        json changes = {{"log_level", request_json["level"]}};
        config_->set(changes, [this](const json& applied) {
            apply_config_side_effects(applied);
        });

        // Return same response format for backward compatibility
        nlohmann::json response = {{"status", "success"}, {"level", config_->log_level()}};
        res.set_content(response.dump(), "application/json");
    } catch (const std::invalid_argument& e) {
        res.status = 400;
        nlohmann::json error = {{"error", e.what()}};
        res.set_content(error.dump(), "application/json");
    } catch (const std::exception& e) {
        LOG(ERROR, "Server") << "ERROR in handle_log_level: " << e.what() << std::endl;
        res.status = 500;
        nlohmann::json error = {{"error", e.what()}};
        res.set_content(error.dump(), "application/json");
    }
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

        // Persist changes to config.json
        if (!cache_dir_.empty()) {
            try {
                ConfigFile::save(cache_dir_, config_->snapshot());
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
                rebind_requested_ = true;
                udp_beacon_.stopBroadcasting();
                stop_http_listeners();
            }
        } else if (key == "host") {
            LOG(INFO, "Server") << "Host change requested to: " << config_->host() << std::endl;
            rebind_requested_ = true;
            udp_beacon_.stopBroadcasting();
            stop_http_listeners();
            // Restart websocket server with new host
            if (websocket_server_) {
                websocket_server_->stop();
                websocket_server_ = std::make_unique<WebSocketServer>(
                    router_.get(),
                    config_->host(),
                    config_->websocket_port());
                if (running_) {
                    websocket_server_->start();
                }
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
        } else if (key == "log_level") {
            std::string level = config_->log_level();
            LOG(INFO, "Server") << "Log level changed to: " << level << std::endl;
            reconfigure_application_logging(level);
        } else if (key == "global_timeout") {
            long timeout = config_->global_timeout();
            LOG(INFO, "Server") << "Global timeout changed to: " << timeout << "s" << std::endl;
            utils::HttpClient::set_default_timeout(timeout);
        } else if (key == "no_broadcast") {
            bool nb = config_->no_broadcast();
            LOG(INFO, "Server") << "Broadcast " << (nb ? "disabled" : "enabled") << std::endl;
            if (nb) {
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
        //    api_key: "..."}  // optional
        if (request_json.value("backend", "") == "cloud") {
            const std::string provider = request_json.value("provider", "");
            const std::string base_url = request_json.value("base_url", "");
            const std::string api_key = request_json.value("api_key", "");
            bool allow_insecure_http = false;
            if (request_json.contains("allow_insecure_http")) {
                if (!request_json["allow_insecure_http"].is_boolean()) {
                    res.status = 400;
                    nlohmann::json error = {{"error", {
                        {"message", "allow_insecure_http must be a boolean when provided"},
                        {"type", "invalid_request_error"}}}};
                    res.set_content(error.dump(), "application/json");
                    return;
                }
                allow_insecure_http = request_json["allow_insecure_http"].get<bool>();
            }
            if (provider.empty() || base_url.empty()) {
                res.status = 400;
                nlohmann::json error = {{"error", {
                    {"message", "Cloud install requires 'provider' and 'base_url' string fields"},
                    {"type", "invalid_request_error"}}}};
                res.set_content(error.dump(), "application/json");
                return;
            }
            if (auto err = CloudProviderRegistry::validate_provider_name(provider); !err.empty()) {
                res.status = 400;
                nlohmann::json error = {{"error", {
                    {"message", err},
                    {"type", "invalid_request_error"}}}};
                res.set_content(error.dump(), "application/json");
                return;
            }
            if (auto err = CloudProviderRegistry::validate_base_url(base_url); !err.empty()) {
                res.status = 400;
                nlohmann::json error = {{"error", {
                    {"message", err},
                    {"type", "invalid_request_error"}}}};
                res.set_content(error.dump(), "application/json");
                return;
            }
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
            cloud_registry_->install(provider, base_url, allow_insecure_http);
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

            nlohmann::json response = {
                {"status", "success"},
                {"backend", "cloud"},
                {"provider", provider},
                {"base_url", cloud_registry_->base_url_for(provider)},
                {"allow_insecure_http", cloud_registry_->allow_insecure_http_for(provider)},
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
