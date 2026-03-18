#include "lemon/websocket_server.h"
#include "lemon/router.h"
#include "lemon/utils/process_manager.h"
#include <iostream>
#include <sstream>
#include <cstring>
#include <cstdio>
#include <lemon/utils/aixlog.hpp>

namespace lemon {

// libwebsockets protocol definition.
// Our WebSocket protocol is listed first so that connections without a
// Sec-WebSocket-Protocol header are routed here by default.
static struct lws_protocols protocols[] = {
    { "lemonade-realtime", WebSocketServer::ws_callback,
      sizeof(PerSessionData), 65536, 0, nullptr, 0 },
    LWS_PROTOCOL_LIST_TERM
};

WebSocketServer::WebSocketServer(Router* router)
    : port_(utils::ProcessManager::find_free_port(9000))
    , router_(router)
    , session_manager_(std::make_unique<RealtimeSessionManager>(router)) {
    LOG(INFO, "WebSocket") << "Allocated port: " << port_ << std::endl;
}

WebSocketServer::~WebSocketServer() {
    stop();
}

int WebSocketServer::ws_callback(struct lws* wsi, enum lws_callback_reasons reason,
                                  void* user, void* in, size_t len) {
    // Get the WebSocketServer instance from context user data
    struct lws_context* ctx = lws_get_context(wsi);
    if (!ctx) return 0;
    auto* server = static_cast<WebSocketServer*>(lws_context_user(ctx));
    if (!server) return 0;

    auto* pss = static_cast<PerSessionData*>(user);

    switch (reason) {
        case LWS_CALLBACK_ESTABLISHED: {
            // Generate connection ID from socket fd (POD-safe char array)
            snprintf(pss->connection_id, sizeof(pss->connection_id),
                     "%d", (int)lws_get_socket_fd(wsi));

            char ip[128] = {0};
            lws_get_peer_simple(wsi, ip, sizeof(ip));
            LOG(INFO, "WebSocket") << "New connection from: " << ip
                      << " (id: " << pss->connection_id << ")" << std::endl;

            server->handle_connection(pss->connection_id, wsi);
            break;
        }

        case LWS_CALLBACK_CLOSED: {
            LOG(INFO, "WebSocket") << "Connection closed: "
                      << pss->connection_id << std::endl;
            server->handle_close(pss->connection_id);
            break;
        }

        case LWS_CALLBACK_RECEIVE: {
            if (in && len > 0) {
                std::string conn_id(pss->connection_id);
                // libwebsockets may deliver a single WebSocket frame across
                // multiple RECEIVE callbacks.  Accumulate fragments and only
                // dispatch when the complete message has arrived.
                {
                    std::lock_guard<std::mutex> lock(server->connections_mutex_);
                    server->receive_buffers_[conn_id].append(
                        static_cast<const char*>(in), len);
                }

                if (lws_remaining_packet_payload(wsi) == 0 &&
                    lws_is_final_fragment(wsi)) {
                    std::string complete_msg;
                    {
                        std::lock_guard<std::mutex> lock(server->connections_mutex_);
                        complete_msg = std::move(server->receive_buffers_[conn_id]);
                        server->receive_buffers_[conn_id].clear();
                    }
                    server->handle_message(conn_id, complete_msg);
                }
            }
            break;
        }

        case LWS_CALLBACK_SERVER_WRITEABLE: {
            server->handle_writable(pss->connection_id, wsi);
            break;
        }

        default:
            break;
    }

    return 0;
}

bool WebSocketServer::start() {
    if (running_.load()) {
        return true;  // Already running
    }

    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));

    info.port = port_;
    info.protocols = protocols;
    info.user = this;  // Store 'this' so static callback can access instance

    // Suppress libwebsockets internal logging (we use our own)
    lws_set_log_level(LLL_ERR | LLL_WARN, nullptr);

    context_ = lws_create_context(&info);
    if (!context_) {
        LOG(ERROR, "WebSocket") << "Failed to create lws context on port " << port_ << std::endl;
        return false;
    }

    running_.store(true);

    // Run the service loop in a background thread
    service_thread_ = std::thread(&WebSocketServer::service_loop, this);

    LOG(INFO, "WebSocket") << "Server started on port " << port_ << std::endl;
    return true;
}

void WebSocketServer::stop() {
    if (!running_.load()) {
        return;
    }

    running_.store(false);

    // Wake up the service thread so it exits the loop
    if (context_) {
        lws_cancel_service(context_);
    }

    // Wait for service thread to finish
    if (service_thread_.joinable()) {
        service_thread_.join();
    }

    // Close all sessions
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        for (auto& [conn_id, session_id] : connection_sessions_) {
            session_manager_->close_session(session_id);
        }
        connection_sessions_.clear();
        connection_websockets_.clear();
        message_queues_.clear();
        receive_buffers_.clear();
    }

    // Destroy context after sessions are cleaned up
    if (context_) {
        lws_context_destroy(context_);
        context_ = nullptr;
    }

    LOG(INFO, "WebSocket") << "Server stopped" << std::endl;
}

void WebSocketServer::service_loop() {
    while (running_.load()) {
        lws_service(context_, 50);  // 50ms timeout to check running_ flag
    }
}

void WebSocketServer::handle_connection(const std::string& connection_id, struct lws* wsi) {
    // Extract URI and query params from the WebSocket handshake
    char uri_buf[256] = {0};
    char query_buf[512] = {0};
    lws_hdr_copy(wsi, uri_buf, sizeof(uri_buf), WSI_TOKEN_GET_URI);
    int query_len = lws_hdr_copy(wsi, query_buf, sizeof(query_buf), WSI_TOKEN_HTTP_URI_ARGS);

    std::string url = std::string(uri_buf);
    if (query_len > 0) {
        url += "?" + std::string(query_buf);
    }

    // Parse query parameters (OpenAI SDK passes ?model=X)
    auto params = parse_query_params(url);

    // Build initial session config from URL params
    json initial_config = json::object();

    // Get model from URL (OpenAI SDK compatible)
    if (params.count("model")) {
        initial_config["model"] = params["model"];
        LOG(INFO, "WebSocket") << "Model from URL: " << params["model"] << std::endl;
    }

    // Store WebSocket pointer for this connection
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        connection_websockets_[connection_id] = wsi;
    }

    // Create session with callback to send messages
    std::string conn_id_copy = connection_id;
    auto send_callback = [this, conn_id_copy](const json& msg) {
        send_json(conn_id_copy, msg);
    };

    std::string session_id = session_manager_->create_session(send_callback, initial_config);

    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        connection_sessions_[connection_id] = session_id;
    }
}

void WebSocketServer::handle_message(const std::string& connection_id, const std::string& msg) {
    // Get session ID for this connection
    std::string session_id;
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        auto it = connection_sessions_.find(connection_id);
        if (it == connection_sessions_.end()) {
            LOG(ERROR, "WebSocket") << "Message from unknown connection" << std::endl;
            return;
        }
        session_id = it->second;
    }

    // Parse JSON message
    json request;
    try {
        request = json::parse(msg);
    } catch (const json::parse_error& e) {
        json error_msg = {
            {"type", "error"},
            {"error", {
                {"message", "Invalid JSON: " + std::string(e.what())},
                {"type", "invalid_request_error"}
            }}
        };
        send_json(connection_id, error_msg);
        return;
    }

    // Get message type
    std::string msg_type = request.value("type", "");

    if (msg_type == "session.update") {
        // Update session configuration
        json session_config = request.value("session", json::object());
        session_manager_->update_session(session_id, session_config);
    }
    else if (msg_type == "input_audio_buffer.append") {
        // Append audio data
        std::string audio = request.value("audio", "");
        if (!audio.empty()) {
            session_manager_->append_audio(session_id, audio);
        }
    }
    else if (msg_type == "input_audio_buffer.commit") {
        // Commit audio buffer (force transcription)
        session_manager_->commit_audio(session_id);
    }
    else if (msg_type == "input_audio_buffer.clear") {
        // Clear audio buffer
        session_manager_->clear_audio(session_id);
    }
    else {
        // Unknown message type
        json error_msg = {
            {"type", "error"},
            {"error", {
                {"message", "Unknown message type: " + msg_type},
                {"type", "invalid_request_error"}
            }}
        };
        send_json(connection_id, error_msg);
    }
}

void WebSocketServer::handle_close(const std::string& connection_id) {
    std::string session_id;
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        auto it = connection_sessions_.find(connection_id);
        if (it != connection_sessions_.end()) {
            session_id = it->second;
            connection_sessions_.erase(it);
        }
        connection_websockets_.erase(connection_id);
        message_queues_.erase(connection_id);
        receive_buffers_.erase(connection_id);
    }

    if (!session_id.empty()) {
        session_manager_->close_session(session_id);
    }
}

void WebSocketServer::handle_writable(const std::string& connection_id, struct lws* wsi) {
    std::string msg;
    bool has_more = false;

    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        auto it = message_queues_.find(connection_id);
        if (it == message_queues_.end() || it->second.empty()) {
            return;
        }
        msg = std::move(it->second.front());
        it->second.pop();
        has_more = !it->second.empty();
    }

    // Allocate buffer with LWS_PRE padding
    std::vector<unsigned char> buf(LWS_PRE + msg.size());
    memcpy(&buf[LWS_PRE], msg.data(), msg.size());

    int written = lws_write(wsi, &buf[LWS_PRE], msg.size(), LWS_WRITE_TEXT);
    if (written < static_cast<int>(msg.size())) {
        LOG(ERROR, "WebSocket") << "Error writing to connection " << connection_id << std::endl;
        return;
    }

    // If there are more messages queued, request another writable callback
    if (has_more) {
        lws_callback_on_writable(wsi);
    }
}

std::unordered_map<std::string, std::string> WebSocketServer::parse_query_params(const std::string& url) {
    std::unordered_map<std::string, std::string> params;

    // Find query string start
    size_t query_start = url.find('?');
    if (query_start == std::string::npos) {
        return params;
    }

    std::string query = url.substr(query_start + 1);

    // Parse key=value pairs
    std::istringstream stream(query);
    std::string pair;

    while (std::getline(stream, pair, '&')) {
        size_t eq_pos = pair.find('=');
        if (eq_pos != std::string::npos) {
            std::string key = pair.substr(0, eq_pos);
            std::string value = pair.substr(eq_pos + 1);
            params[key] = value;
        }
    }

    return params;
}

void WebSocketServer::send_json(const std::string& connection_id, const json& msg) {
    try {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        auto it = connection_websockets_.find(connection_id);
        if (it != connection_websockets_.end() && it->second != nullptr) {
            // Queue the message for deferred writing
            message_queues_[connection_id].push(msg.dump());
            // Request a writable callback for this connection
            lws_callback_on_writable(it->second);
        }
    } catch (const std::exception& e) {
        LOG(ERROR, "WebSocket") << "Error sending message to " << connection_id
                  << ": " << e.what() << std::endl;
    }

    // Wake up the service thread to process the writable callback
    if (context_) {
        lws_cancel_service(context_);
    }
}

} // namespace lemon
