#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <httplib.h>

#include "lemon/upgradable_http_server.h"
#include "lemon/utils/network_beacon.h"

namespace lemon {

class Router;
class RuntimeConfig;
class WebSocketServer;

// Owns everything that puts lemond on the network: the IPv4 and IPv6 listeners and
// their thread pools, the UDP beacon, and the WebSocket server. A host or port change
// rebinds them in place, so lemond keeps running across the change.
class HttpListener {
public:
    // setup registers middleware, routes and the web UI on each server the listener
    // creates, including the fresh ones a rebind creates.
    using SetupFn = std::function<void(httplib::Server&)>;

    // A shutdown request or a failed listener stops all of lemond, not just the listener:
    // stop_server unloads the models, which ends in-flight requests that would otherwise
    // keep the listener threads from finishing.
    HttpListener(RuntimeConfig* config, Router* router, SetupFn setup,
                 std::function<void()> stop_server, bool api_key_set, bool admin_api_key_set);
    ~HttpListener();

    // Binds and serves until stop() or request_shutdown(), rebinding whenever rebind()
    // asks.
    void start();

    // Returns whether the listener was running.
    bool stop();

    // Applies the host, port, websocket_port and broadcast settings from RuntimeConfig to
    // the running listeners. Only what changed is restarted.
    void rebind();

    // 0 when the WebSocket server is not running.
    int websocket_port() const;
    int port() const { return port_.load(); }
    bool is_running() const { return running_.load(); }
    bool startup_failed() const { return startup_failed_; }

    void request_shutdown() { shutdown_requested_.store(true); }
    bool shutdown_requested() const { return shutdown_requested_.load(); }

private:
    void create_servers();
    void stop_servers();
    void join_threads();
    void start_beacon();
    void restart_websocket();
    void warn_if_unsecured(const std::string& host, const std::string& ipv4,
                           const std::string& ipv6) const;
    static std::string resolve_host_to_ip(int ai_family, const std::string& host);

    RuntimeConfig* config_;
    Router* router_;
    SetupFn setup_;
    std::function<void()> stop_server_;
    bool api_key_set_;
    bool admin_api_key_set_;

    // Concurrent config changes each call rebind(), which compares and updates the settings
    // below.
    std::mutex rebind_mutex_;
    std::atomic<int> port_;
    std::string bound_host_;
    int websocket_requested_port_;
    bool broadcast_;

    // The routed servers carry every route and never listen; the fronts own the main
    // port and hand WebSocket upgrades to websocket_server_ (see upgradable_http_server.h).
    std::unique_ptr<RoutedHttpServer> http_server_;
    std::unique_ptr<RoutedHttpServer> http_server_v6_;
    std::unique_ptr<UpgradableFrontServer> http_front_;
    std::unique_ptr<UpgradableFrontServer> http_front_v6_;
    std::thread http_v4_thread_;
    std::thread http_v6_thread_;

    // A host or websocket_port change replaces the WebSocket server while request threads
    // read its port, so it is only touched under this mutex.
    mutable std::mutex websocket_mutex_;
    std::unique_ptr<WebSocketServer> websocket_server_;

    NetworkBeacon udp_beacon_;

    std::atomic<bool> running_{false};
    std::atomic<bool> shutdown_requested_{false};
    std::atomic<bool> rebind_requested_{false};
    bool startup_failed_ = false;
};

} // namespace lemon
