#pragma once

#include <string>
#include <memory>
#include <atomic>
#include <thread>
#include <unordered_map>
#include <mutex>
#include <functional>
#include <ixwebsocket/IXWebSocketServer.h>
#include <nlohmann/json.hpp>
#include "realtime_session.h"

namespace lemon {

using json = nlohmann::json;

// Forward declaration
class Router;

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

private:
    int port_;
    Router* router_;
    std::unique_ptr<RealtimeSessionManager> session_manager_;
    ix::WebSocketServer ws_server_;
    std::atomic<bool> running_{false};

    // Map connection IDs to session IDs
    std::unordered_map<std::string, std::string> connection_sessions_;
    // Map connection IDs to WebSocket references for sending
    std::unordered_map<std::string, ix::WebSocket*> connection_websockets_;
    std::mutex connections_mutex_;

    // Handle new WebSocket connection
    void handle_connection(const std::string& connection_id, ix::WebSocket* ws, const std::string& url);

    // Handle incoming WebSocket message
    void handle_message(const std::string& connection_id, const std::string& msg);

    // Handle WebSocket connection close
    void handle_close(const std::string& connection_id);

    // Parse URL query parameters
    static std::unordered_map<std::string, std::string> parse_query_params(const std::string& url);

    // Send JSON message to WebSocket by connection ID
    void send_json(const std::string& connection_id, const json& msg);
};

} // namespace lemon
