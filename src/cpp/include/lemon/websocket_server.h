#pragma once

#include <string>
#include <memory>
#include <atomic>
#include <thread>
#include <unordered_map>
#include <mutex>
#include <functional>
#include <optional>
#include <queue>
#include <libwebsockets.h>
#include <nlohmann/json.hpp>
#include "log_stream.h"
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
 * Shared WebSocket server for realtime audio transcription and log streaming.
 * Multiplexes /realtime (OpenAI Realtime API) and /logs/stream endpoints.
 */
class WebSocketServer {
public:
    WebSocketServer(Router* router, const std::string& host, int requested_port);
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
    enum class ConnectionKind {
        invalid,
        realtime,
        logs,
    };

    struct ConnectionState {
        ConnectionKind kind = ConnectionKind::invalid;
        std::string realtime_session_id;
        std::string log_subscriber_id;
    };

    int port_;
    std::string host_;
    std::string api_key_;
    std::string admin_api_key_;
    Router* router_;
    std::unique_ptr<RealtimeSessionManager> session_manager_;
    struct lws_context* context_{nullptr};
    std::thread service_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> writable_dispatch_pending_{false};

    std::unordered_map<std::string, ConnectionState> connection_states_;
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

    bool authenticate_connection(struct lws* wsi) const;
    static std::optional<std::string> get_header(struct lws* wsi, enum lws_token_indexes token);
    static std::optional<std::string> get_url_arg(struct lws* wsi, const char* name);
    static std::string get_request_path(struct lws* wsi);
    static ConnectionKind classify_path(const std::string& path);

    // Send JSON message to WebSocket by connection ID
    void send_json(const std::string& connection_id, const json& msg);

    void handle_log_subscribe(const std::string& connection_id,
                              std::optional<uint64_t> after_seq);
    void handle_realtime_connection(const std::string& connection_id,
                                    struct lws* wsi);
    void schedule_pending_writes();

    // Service loop run in background thread
    void service_loop();
};

} // namespace lemon
