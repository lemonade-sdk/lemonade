#include "lemon/websocket_server.h"
#include "lemon/router.h"
#include "lemon/utils/process_manager.h"
#include <iostream>
#include <sstream>

namespace lemon {

WebSocketServer::WebSocketServer(Router* router)
    : port_(utils::ProcessManager::find_free_port(9000))  // Use 9000+ to avoid backend subprocess ports (8001+)
    , router_(router)
    , session_manager_(std::make_unique<RealtimeSessionManager>(router))
    , ws_server_(port_, "0.0.0.0") {
    std::cout << "[WebSocket] Allocated port: " << port_ << std::endl;
}

WebSocketServer::~WebSocketServer() {
    stop();
}

bool WebSocketServer::start() {
    if (running_.load()) {
        return true;  // Already running
    }

    // Set up connection handler
    ws_server_.setOnClientMessageCallback(
        [this](std::shared_ptr<ix::ConnectionState> connectionState,
               ix::WebSocket& webSocket,
               const ix::WebSocketMessagePtr& msg) {

            std::string conn_id = connectionState->getId();

            switch (msg->type) {
                case ix::WebSocketMessageType::Open: {
                    std::cout << "[WebSocket] New connection from: "
                              << connectionState->getRemoteIp()
                              << " (id: " << conn_id << ")" << std::endl;

                    handle_connection(conn_id, &webSocket, msg->openInfo.uri);
                    break;
                }

                case ix::WebSocketMessageType::Close: {
                    std::cout << "[WebSocket] Connection closed: "
                              << connectionState->getRemoteIp() << std::endl;
                    handle_close(conn_id);
                    break;
                }

                case ix::WebSocketMessageType::Message: {
                    if (msg->binary) {
                        // Binary messages not supported for now
                        std::cerr << "[WebSocket] Received unsupported binary message" << std::endl;
                    } else {
                        handle_message(conn_id, msg->str);
                    }
                    break;
                }

                case ix::WebSocketMessageType::Error: {
                    std::cerr << "[WebSocket] Error: " << msg->errorInfo.reason << std::endl;
                    break;
                }

                case ix::WebSocketMessageType::Ping:
                case ix::WebSocketMessageType::Pong:
                case ix::WebSocketMessageType::Fragment:
                    // Handled automatically by IXWebSocket
                    break;
            }
        }
    );

    // Configure server options
    ws_server_.disablePerMessageDeflate();

    // Start listening
    auto result = ws_server_.listen();
    if (!result.first) {
        std::cerr << "[WebSocket] Failed to start server on port " << port_
                  << ": " << result.second << std::endl;
        return false;
    }

    // Start the server
    ws_server_.start();
    running_.store(true);

    std::cout << "[WebSocket] Server started on port " << port_ << std::endl;
    return true;
}

void WebSocketServer::stop() {
    if (!running_.load()) {
        return;
    }

    running_.store(false);
    ws_server_.stop();

    // Close all sessions
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        for (auto& [conn_id, session_id] : connection_sessions_) {
            session_manager_->close_session(session_id);
        }
        connection_sessions_.clear();
        connection_websockets_.clear();
    }

    std::cout << "[WebSocket] Server stopped" << std::endl;
}

void WebSocketServer::handle_connection(const std::string& connection_id, ix::WebSocket* ws, const std::string& url) {
    // Parse query parameters (OpenAI SDK passes ?model=X)
    auto params = parse_query_params(url);

    // Build initial session config from URL params
    json initial_config = json::object();

    // Get model from URL (OpenAI SDK compatible)
    if (params.count("model")) {
        initial_config["model"] = params["model"];
        std::cout << "[WebSocket] Model from URL: " << params["model"] << std::endl;
    }

    // Store WebSocket pointer for this connection
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        connection_websockets_[connection_id] = ws;
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
            std::cerr << "[WebSocket] Message from unknown connection" << std::endl;
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
    }

    if (!session_id.empty()) {
        session_manager_->close_session(session_id);
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
            it->second->send(msg.dump());
        }
    } catch (const std::exception& e) {
        std::cerr << "[WebSocket] Error sending message to " << connection_id
                  << ": " << e.what() << std::endl;
    }
}

} // namespace lemon
