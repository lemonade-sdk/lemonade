#pragma once

#include <string>
#include <memory>
#include <atomic>
#include <thread>
#include <unordered_map>
#include <mutex>
#include <functional>
#include <queue>
#include <libwebsockets.h>
#include <nlohmann/json.hpp>
#include "realtime_session.h"

namespace lemon {

using json = nlohmann::json;

// Forward declaration
class Router;

/**
 * Per-session data allocated by libwebsockets for each connection.
 * Must be a POD type — libwebsockets uses malloc/free, not new/delete.
 */
struct PerSessionData {
    char connection_id[32];
};

/**
 * WebSocket server for realtime audio transcription.
 * Implements OpenAI-compatible Realtime API message protocol.
 */
class WebSocketServer {
public:
    explicit WebSocketServer(Router* router);
    ~WebSocketServer();

    // Non-copyable
    WebSocketServer(const WebSocketServer&) = delete;
    WebSocketServer& operator=(const WebSocketServer&) = delete;

    /**
     * Start the WebSocket server.
     * Binds to an OS-assigned port (port 0).
     * @return true if started successfully
     */
    bool start();

    /**
     * Stop the WebSocket server.
     */
    void stop();

    /**
     * Check if the server is running.
     */
    bool is_running() const { return running_.load(); }

    /**
     * Get the server port.
     * Only valid after start() returns true.
     */
    int get_port() const { return port_; }

    // libwebsockets callback (public — referenced by file-scope protocols array)
    static int ws_callback(struct lws* wsi, enum lws_callback_reasons reason,
                           void* user, void* in, size_t len);

private:
    int port_;
    Router* router_;
    std::unique_ptr<RealtimeSessionManager> session_manager_;
    struct lws_context* context_{nullptr};
    std::thread service_thread_;
    std::atomic<bool> running_{false};

    // Map connection IDs to session IDs
    std::unordered_map<std::string, std::string> connection_sessions_;
    // Map connection IDs to lws wsi pointers for sending
    std::unordered_map<std::string, struct lws*> connection_websockets_;
    // Per-connection outbound message queues (deferred write pattern)
    std::unordered_map<std::string, std::queue<std::string>> message_queues_;
    // Per-connection inbound reassembly buffers (libwebsockets may fragment frames)
    std::unordered_map<std::string, std::string> receive_buffers_;
    std::mutex connections_mutex_;

    // Handle new WebSocket connection
    void handle_connection(const std::string& connection_id, struct lws* wsi);

    // Handle incoming WebSocket message
    void handle_message(const std::string& connection_id, const std::string& msg);

    // Handle WebSocket connection close
    void handle_close(const std::string& connection_id);

    // Handle writable callback — flush message queue
    void handle_writable(const std::string& connection_id, struct lws* wsi);

    // Parse URL query parameters
    static std::unordered_map<std::string, std::string> parse_query_params(const std::string& url);

    // Send JSON message to WebSocket by connection ID
    void send_json(const std::string& connection_id, const json& msg);

    // Service loop run in background thread
    void service_loop();
};

} // namespace lemon
