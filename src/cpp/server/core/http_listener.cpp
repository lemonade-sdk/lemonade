#include "lemon/server/http_listener.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>

#include <lemon/utils/aixlog.hpp>

#include "lemon/runtime_config.h"
#include "lemon/utils/network_utils.h"
#include "lemon/websocket_server.h"

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
#else
    #include <arpa/inet.h>
    #include <netdb.h>
    #include <netinet/in.h>
    #include <sys/socket.h>
    #include <sys/types.h>
#endif

// Extract the member-function pointer for httplib::Server's private virtual
// process_and_close_socket (see upgradable_http_server.h). Explicit
// instantiation is the one context where C++ permits naming a private member.
template struct lemon::detail::PrivateMemberInit<
    lemon::detail::ProcessAndCloseSocketTag,
    &httplib::Server::process_and_close_socket>;

namespace lemon {

namespace {

// Clients find lemond by listening on this port, so it is not configurable.
constexpr int kBeaconPort = 13305;

} // namespace

HttpListener::HttpListener(RuntimeConfig* config, Router* router, SetupFn setup,
                           std::function<void()> stop_server, bool api_key_set,
                           bool admin_api_key_set)
    : config_(config),
      router_(router),
      setup_(std::move(setup)),
      stop_server_(std::move(stop_server)),
      api_key_set_(api_key_set),
      admin_api_key_set_(admin_api_key_set),
      port_(config->port()),
      bound_host_(config->host()),
      websocket_requested_port_(config->websocket_port()),
      broadcast_(config->broadcast()) {
    create_servers();
    websocket_server_ = std::make_unique<WebSocketServer>(
        router_, config_->host(), config_->websocket_port());
}

HttpListener::~HttpListener() {
    stop();
}

void HttpListener::create_servers() {
    http_server_ = std::make_unique<RoutedHttpServer>();
    http_server_v6_ = std::make_unique<RoutedHttpServer>();

    // Front listeners for the main port: WebSocket upgrades for /realtime and
    // /logs/stream are adopted by the libwebsockets server; everything else is
    // processed by the routed servers above. The dedicated websocket_port
    // listener keeps running unchanged.
    auto upgrade_handler = [this](socket_t sock) -> bool {
        std::lock_guard<std::mutex> lock(websocket_mutex_);
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
    // port_is_available() in start(), not by making these listeners exclusive.

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

    setup_(*http_server_);
    setup_(*http_server_v6_);
}

void HttpListener::stop_servers() {
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

void HttpListener::join_threads() {
    if (http_v4_thread_.joinable())
        http_v4_thread_.join();
    if (http_v6_thread_.joinable())
        http_v6_thread_.join();
}

std::string HttpListener::resolve_host_to_ip(int ai_family, const std::string& host) {
    struct addrinfo hints = {0};
    hints.ai_family = ai_family;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = 0; // No AI_ADDRCONFIG: allows loopback resolution when offline

    struct addrinfo *result = nullptr;

    if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0) {
        LOG(WARNING, "Server") << "resolution failed for " << host << " no " << (ai_family == AF_INET ? "IPv4" : ai_family == AF_INET6 ? "IPv6" : "") << " resolution found." << std::endl;
        return "";
    }

    if (result == nullptr) return "";

    // Use INET6_ADDRSTRLEN to be safe for both (it's larger)
    char addrstr[INET6_ADDRSTRLEN];
    void *ptr = nullptr;

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

    inet_ntop(result->ai_family, ptr, addrstr, sizeof(addrstr));

    std::string resolved_ip(addrstr);
    freeaddrinfo(result);
    return resolved_ip;
}

// Operators binding beyond loopback should secure the server with an API
// key, since every endpoint is reachable from other machines once the host
// is non-loopback. The regular API routes (/api, /v0, /v1) are gated by
// LEMONADE_API_KEY; the /internal/* control endpoints (shutdown, set, config) are
// gated by LEMONADE_ADMIN_API_KEY, which defaults to the regular key. Setting only
// LEMONADE_ADMIN_API_KEY therefore protects /internal/* but still leaves the
// inference and model-management endpoints exposed, so we warn unless the
// regular key is set.
void HttpListener::warn_if_unsecured(const std::string& bound_host, const std::string& v4,
                                     const std::string& v6) const {
    auto is_loopback = [](const std::string& ip) {
        return ip.empty() || ip.rfind("127.", 0) == 0 || ip == "::1";
    };
    if (is_loopback(v4) && is_loopback(v6)) {
        return;
    }
    if (!api_key_set_ && !admin_api_key_set_) {
        LOG(WARNING, "Server")
            << "Serving on non-loopback host '" << bound_host
            << "' without an API key. All endpoints, including the /internal/* "
               "control endpoints, are reachable from other machines "
               "unauthenticated. Set LEMONADE_API_KEY to secure all endpoints; "
               "LEMONADE_ADMIN_API_KEY on its own only secures the /internal/* "
               "control endpoints." << std::endl;
    } else if (!api_key_set_) {
        LOG(WARNING, "Server")
            << "Serving on non-loopback host '" << bound_host
            << "' with only an admin API key set. The /internal/* control "
               "endpoints are protected, but the inference and model-management "
               "endpoints (/api, /v0, /v1) are reachable from other machines "
               "unauthenticated. Set LEMONADE_API_KEY to secure them." << std::endl;
    }
}

void HttpListener::start_beacon() {
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
        udp_beacon_.startBroadcasting(kBeaconPort, port_, 2);
    } else if (!rfc1918Interfaces.empty() && !bcast) {
        LOG(INFO, "Server") << "Broadcasting disabled by configuration or CLI option" << std::endl;
    } else {
        LOG(INFO, "Server") << "Unable to broadcast my existance please use a RFC1918 IPv4," << std::endl
                    << "or hostname that resolves to RFC1918 IPv4." << std::endl;
    }
}

void HttpListener::start() {
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

    warn_if_unsecured(host, ipv4, ipv6);

    running_ = true;

    {
        std::lock_guard<std::mutex> lock(websocket_mutex_);
        if (websocket_server_) {
            if (websocket_server_->start()) {
                LOG(INFO, "Server") << "WebSocket server started on port "
                                    << websocket_server_->get_port() << std::endl;
            } else {
                LOG(WARNING, "Server") << "Failed to start WebSocket server" << std::endl;
            }
        }
    }

    while (true) {
        if (shutdown_requested_.load()) {
            LOG(INFO, "Server") << "Shutdown requested, stopping server..." << std::endl;
            stop_server_();
            break;
        }

        const std::size_t listener_count =
            static_cast<std::size_t>(!ipv4.empty()) +
            static_cast<std::size_t>(!ipv6.empty());
        utils::ListenerStartupState listener_startup(listener_count);

        if (!ipv4.empty()) {
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
            stop_server_();
            join_threads();
            startup_failed_ = true;
            break;
        }

        start_beacon();

        // Wait for listener threads, but check periodically for shutdown or rebind signals.
        // The threads are blocked in listen_after_bind(), which only returns when
        // the server is stopped or an error occurs.
        while ((http_v4_thread_.joinable() || http_v6_thread_.joinable()) &&
               !shutdown_requested_.load() && !rebind_requested_.load() &&
               !listener_startup.failed()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        if (shutdown_requested_.load()) {
            LOG(INFO, "Server") << "Shutdown requested, stopping server..." << std::endl;
            stop_server_();
            join_threads();
            break;
        }

        if (listener_startup.failed()) {
            LOG(ERROR, "Server") << "An HTTP listener stopped unexpectedly. Server exiting." << std::endl;
            stop_server_();
            join_threads();
            startup_failed_ = true;
            break;
        }

        // A rebind has already stopped the listeners; otherwise they exited because
        // stop() was called. Either way the threads are done.
        join_threads();

        if (!rebind_requested_) {
            break;
        }

        // Rebind requested: re-resolve host, recreate HTTP servers, loop back to bind+listen
        host = config_->host();
        ipv4 = resolve_host_to_ip(AF_INET, host);
        ipv6 = resolve_host_to_ip(AF_INET6, host);
        warn_if_unsecured(host, ipv4, ipv6);
        LOG(INFO, "Server") << "Rebinding to " << host << ":" << port_ << "..." << std::endl;
        rebind_requested_ = false;
        create_servers();
    }
}


bool HttpListener::stop() {
    if (!running_.exchange(false)) {
        return false;
    }
    LOG(INFO, "Server") << "Stopping HTTP server..." << std::endl;
    udp_beacon_.stopBroadcasting();
    stop_servers();
    shutdown_requested_ = false;  // Reset for potential future use

    std::lock_guard<std::mutex> lock(websocket_mutex_);
    if (websocket_server_) {
        LOG(INFO, "Server") << "Stopping WebSocket server..." << std::endl;
        websocket_server_->stop();
    }
    return true;
}

void HttpListener::restart_websocket() {
    std::lock_guard<std::mutex> lock(websocket_mutex_);
    if (!websocket_server_) {
        return;
    }
    websocket_server_->stop();
    websocket_server_ = std::make_unique<WebSocketServer>(
        router_, config_->host(), config_->websocket_port());
    if (running_) {
        websocket_server_->start();
    }
}

void HttpListener::rebind() {
    std::lock_guard<std::mutex> lock(rebind_mutex_);
    const int new_port = config_->port();
    const std::string new_host = config_->host();
    const int new_websocket_port = config_->websocket_port();

    bool rebind_http = false;
    bool restart_ws = false;

    const int current_port = port_.load();
    if (new_port != current_port) {
        LOG(INFO, "Server") << "Port change requested: " << current_port << " -> " << new_port << std::endl;
        port_.store(new_port);
        rebind_http = true;
    }
    if (new_host != bound_host_) {
        LOG(INFO, "Server") << "Host change requested to: " << new_host << std::endl;
        bound_host_ = new_host;
        rebind_http = true;
        restart_ws = running_.load();
    }
    if (new_websocket_port != websocket_requested_port_) {
        LOG(INFO, "Server") << "Restarting WebSocket server on requested port "
                            << new_websocket_port << std::endl;
        websocket_requested_port_ = new_websocket_port;
        restart_ws = true;
    }

    if (rebind_http && running_) {
        rebind_requested_ = true;
        udp_beacon_.stopBroadcasting();
        stop_servers();
    }
    if (restart_ws) {
        restart_websocket();
    }

    const bool broadcast = config_->broadcast();
    if (broadcast != broadcast_) {
        broadcast_ = broadcast;
        LOG(INFO, "Server") << "Broadcast " << (broadcast ? "enabled" : "disabled") << std::endl;
        if (!broadcast) {
            udp_beacon_.stopBroadcasting();
        } else if (!udp_beacon_.getLocalRFC1918Interfaces().empty()) {
            udp_beacon_.startBroadcasting(kBeaconPort, port_, 2);
        }
    }
}

int HttpListener::websocket_port() const {
    std::lock_guard<std::mutex> lock(websocket_mutex_);
    if (websocket_server_ && websocket_server_->is_running()) {
        return websocket_server_->get_port();
    }
    return 0;
}

} // namespace lemon
