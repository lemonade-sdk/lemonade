// Copyright (c) 2025 AMD
// SPDX-License-Identifier: Apache-2.0

#include "websocket_handler.h"
#include "lemon/router.h"
#include "lemon/audio_buffer.h"

#include <App.h>  // uWebSockets main header

#include <iostream>
#include <sstream>
#include <fstream>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <filesystem>
#include <cstdlib>
#include <unordered_map>
#include <atomic>
#include <memory>

namespace lemon {

// Base64 decoding helper
static std::vector<uint8_t> base64_decode(const std::string& encoded) {
    static const std::string chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::vector<uint8_t> decoded;
    std::vector<int> T(256, -1);
    for (int i = 0; i < 64; i++) T[chars[i]] = i;

    int val = 0, valb = -8;
    for (unsigned char c : encoded) {
        if (T[c] == -1) break;
        val = (val << 6) + T[c];
        valb += 6;
        if (valb >= 0) {
            decoded.push_back(static_cast<uint8_t>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return decoded;
}

AudioStreamMessage AudioStreamMessage::from_json(const nlohmann::json& j) {
    AudioStreamMessage msg;
    msg.type = j.value("type", "");
    msg.model = j.value("model", "");
    msg.language = j.value("language", "");
    msg.data = j.value("data", "");
    msg.sample_rate = j.value("sample_rate", 16000);
    return msg;
}

nlohmann::json AudioStreamResponse::to_json() const {
    nlohmann::json j;
    j["type"] = type;
    if (!text.empty()) j["text"] = text;
    j["is_final"] = is_final;
    if (!message.empty()) j["message"] = message;
    if (timestamp > 0) j["timestamp"] = timestamp;
    return j;
}

/**
 * @brief Per-connection state for audio streaming
 * Note: uWebSockets requires this to be default-constructible and copyable
 */
struct ConnectionState {
    uint64_t connection_id = 0;
};

/**
 * @brief Extended connection data (not stored in ConnectionState directly)
 */
struct ConnectionData {
    std::string model;
    std::string language;
    std::unique_ptr<AudioBuffer> audio_buffer;
    bool streaming_active = false;
    std::mutex mutex;
};

/**
 * @brief Internal implementation using uWebSockets
 */
class WebSocketHandler::Impl {
public:
    Impl(Router& router, int port)
        : router_(router), port_(port) {}

    ~Impl() {
        stop();
    }

    bool start() {
        if (running_) return true;

        // Create uWebSockets app
        uWS::App app;

        // Configure WebSocket behavior
        app.ws<ConnectionState>("/api/v1/audio/stream", {
            // Settings
            .compression = uWS::DISABLED,
            .maxPayloadLength = 16 * 1024 * 1024,  // 16 MB max message
            .idleTimeout = 120,  // 2 minute idle timeout
            .maxBackpressure = 1 * 1024 * 1024,  // 1 MB backpressure limit

            // Upgrade handler (HTTP -> WebSocket)
            .upgrade = [this](auto* res, auto* req, auto* context) {
                ConnectionState state;
                state.connection_id = next_connection_id_++;
                // Create connection data
                {
                    std::lock_guard<std::mutex> lock(connections_mutex_);
                    connections_[state.connection_id] = std::make_unique<ConnectionData>();
                }
                res->template upgrade<ConnectionState>(
                    std::move(state),
                    req->getHeader("sec-websocket-key"),
                    req->getHeader("sec-websocket-protocol"),
                    req->getHeader("sec-websocket-extensions"),
                    context
                );
            },

            // Connection opened
            .open = [this](auto* ws) {
                std::cout << "[WebSocket] Client connected" << std::endl;
            },

            // Message received
            .message = [this](auto* ws, std::string_view message, uWS::OpCode opCode) {
                handle_message(ws, message, opCode);
            },

            // Connection closed
            .close = [this](auto* ws, int code, std::string_view message) {
                std::cout << "[WebSocket] Client disconnected: " << code << std::endl;
                auto* state = ws->getUserData();
                // Clean up connection data
                std::lock_guard<std::mutex> lock(connections_mutex_);
                connections_.erase(state->connection_id);
            }
        });

        // Also support v0 API
        app.ws<ConnectionState>("/api/v0/audio/stream", {
            .compression = uWS::DISABLED,
            .maxPayloadLength = 16 * 1024 * 1024,
            .idleTimeout = 120,
            .maxBackpressure = 1 * 1024 * 1024,
            .upgrade = [this](auto* res, auto* req, auto* context) {
                ConnectionState state;
                state.connection_id = next_connection_id_++;
                {
                    std::lock_guard<std::mutex> lock(connections_mutex_);
                    connections_[state.connection_id] = std::make_unique<ConnectionData>();
                }
                res->template upgrade<ConnectionState>(
                    std::move(state),
                    req->getHeader("sec-websocket-key"),
                    req->getHeader("sec-websocket-protocol"),
                    req->getHeader("sec-websocket-extensions"),
                    context
                );
            },
            .open = [](auto* ws) {},
            .message = [this](auto* ws, std::string_view message, uWS::OpCode opCode) {
                handle_message(ws, message, opCode);
            },
            .close = [this](auto* ws, int code, std::string_view message) {
                auto* state = ws->getUserData();
                std::lock_guard<std::mutex> lock(connections_mutex_);
                connections_.erase(state->connection_id);
            }
        });

        // Listen on specified port
        app.listen(port_, [this](auto* listen_socket) {
            if (listen_socket) {
                listen_socket_ = listen_socket;
                running_ = true;
                std::cout << "[WebSocket] Audio streaming server listening on port " << port_ << std::endl;
            } else {
                std::cerr << "[WebSocket] Failed to listen on port " << port_ << std::endl;
            }
        });

        if (!running_) {
            return false;
        }

        // Store the app loop for later stopping
        loop_ = uWS::Loop::get();

        // Run the event loop (blocking)
        app.run();

        return true;
    }

    void stop() {
        if (!running_) return;

        running_ = false;

        // Close the listen socket to stop accepting new connections
        if (listen_socket_ && loop_) {
            loop_->defer([this]() {
                if (listen_socket_) {
                    us_listen_socket_close(0, listen_socket_);
                    listen_socket_ = nullptr;
                }
            });
        }
    }

    bool is_running() const { return running_; }

private:
    ConnectionData* get_connection_data(uint64_t connection_id) {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        auto it = connections_.find(connection_id);
        if (it != connections_.end()) {
            return it->second.get();
        }
        return nullptr;
    }

    template<bool SSL>
    void handle_message(uWS::WebSocket<SSL, true, ConnectionState>* ws,
                       std::string_view message, uWS::OpCode opCode) {
        auto* state = ws->getUserData();
        auto* conn = get_connection_data(state->connection_id);
        if (!conn) {
            send_error(ws, "Connection not found");
            return;
        }

        try {
            // Parse JSON message
            nlohmann::json j = nlohmann::json::parse(message);
            auto msg = AudioStreamMessage::from_json(j);

            if (msg.type == "start") {
                handle_start(ws, conn, msg);
            } else if (msg.type == "audio_chunk") {
                handle_audio_chunk(ws, conn, msg);
            } else if (msg.type == "stop") {
                handle_stop(ws, conn);
            } else {
                send_error(ws, "Unknown message type: " + msg.type);
            }
        } catch (const nlohmann::json::parse_error& e) {
            send_error(ws, "Invalid JSON: " + std::string(e.what()));
        } catch (const std::exception& e) {
            send_error(ws, "Error processing message: " + std::string(e.what()));
        }
    }

    template<bool SSL>
    void handle_start(uWS::WebSocket<SSL, true, ConnectionState>* ws,
                     ConnectionData* conn, const AudioStreamMessage& msg) {
        std::lock_guard<std::mutex> lock(conn->mutex);

        if (msg.model.empty()) {
            send_error(ws, "Model name is required");
            return;
        }

        // Auto-load the model if not already loaded
        try {
            router_.auto_load_model_if_needed(msg.model);
        } catch (const std::exception& e) {
            send_error(ws, std::string("Failed to load model: ") + e.what());
            return;
        }

        conn->model = msg.model;
        conn->language = msg.language;
        conn->audio_buffer = std::make_unique<AudioBuffer>(msg.sample_rate);
        conn->streaming_active = true;

        std::cout << "[WebSocket] Started streaming for model: " << msg.model << std::endl;

        // Send ready response
        AudioStreamResponse resp;
        resp.type = "ready";
        resp.text = "";
        resp.is_final = false;
        ws->send(resp.to_json().dump(), uWS::OpCode::TEXT);
    }

    template<bool SSL>
    void handle_audio_chunk(uWS::WebSocket<SSL, true, ConnectionState>* ws,
                           ConnectionData* conn, const AudioStreamMessage& msg) {
        std::lock_guard<std::mutex> lock(conn->mutex);

        if (!conn->streaming_active) {
            send_error(ws, "Streaming not started. Send 'start' message first.");
            return;
        }

        // Decode base64 audio data
        auto audio_data = base64_decode(msg.data);
        if (audio_data.empty()) {
            return;  // Ignore empty chunks
        }

        // Add to audio buffer
        conn->audio_buffer->add_chunk(audio_data.data(), audio_data.size());

        // Check if we have enough audio to transcribe (e.g., 3 seconds)
        if (conn->audio_buffer->has_enough_audio()) {
            // Get accumulated audio (but don't clear - keep for context)
            auto audio = conn->audio_buffer->peek_audio();
            double duration = conn->audio_buffer->get_duration();

            // Save audio to temp WAV file
            std::string temp_path = std::filesystem::temp_directory_path().string() +
                                   "/lemonade_stream_" + std::to_string(std::rand()) + ".wav";
            conn->audio_buffer->save_to_wav(temp_path);

            // Create transcription request for Router
            nlohmann::json request;
            request["model"] = conn->model;
            if (!conn->language.empty()) {
                request["language"] = conn->language;
            }

            // Read the WAV file content for the request
            std::ifstream file(temp_path, std::ios::binary);
            std::ostringstream oss;
            oss << file.rdbuf();
            request["file_data"] = oss.str();
            request["filename"] = "stream_audio.wav";
            file.close();

            // Perform transcription via Router
            try {
                nlohmann::json result = router_.audio_transcriptions(request);

                // Send partial result
                AudioStreamResponse resp;
                resp.type = "partial";
                if (result.contains("text")) {
                    resp.text = result["text"].get<std::string>();
                } else if (result.contains("error")) {
                    resp.type = "error";
                    resp.message = result["error"]["message"].get<std::string>();
                }
                resp.is_final = false;
                resp.timestamp = duration;
                ws->send(resp.to_json().dump(), uWS::OpCode::TEXT);
            } catch (const std::exception& e) {
                AudioStreamResponse resp;
                resp.type = "error";
                resp.message = std::string("Transcription failed: ") + e.what();
                ws->send(resp.to_json().dump(), uWS::OpCode::TEXT);
            }

            // Clean up temp file
            std::filesystem::remove(temp_path);

            // Clear the buffer after successful transcription
            conn->audio_buffer->get_audio_for_transcription();
        }
    }

    template<bool SSL>
    void handle_stop(uWS::WebSocket<SSL, true, ConnectionState>* ws,
                    ConnectionData* conn) {
        std::lock_guard<std::mutex> lock(conn->mutex);

        if (!conn->streaming_active) {
            return;
        }

        conn->streaming_active = false;

        // Process any remaining audio
        if (conn->audio_buffer && conn->audio_buffer->get_duration() > 0.5) {
            double duration = conn->audio_buffer->get_duration();

            // Save remaining audio to temp WAV file
            std::string temp_path = std::filesystem::temp_directory_path().string() +
                                   "/lemonade_final_" + std::to_string(std::rand()) + ".wav";
            conn->audio_buffer->save_to_wav(temp_path);

            // Create transcription request
            nlohmann::json request;
            request["model"] = conn->model;
            if (!conn->language.empty()) {
                request["language"] = conn->language;
            }

            // Read the WAV file content
            std::ifstream file(temp_path, std::ios::binary);
            std::ostringstream oss;
            oss << file.rdbuf();
            request["file_data"] = oss.str();
            request["filename"] = "final_audio.wav";
            file.close();

            // Perform final transcription
            AudioStreamResponse resp;
            resp.type = "final";
            resp.is_final = true;
            resp.timestamp = duration;

            try {
                nlohmann::json result = router_.audio_transcriptions(request);
                if (result.contains("text")) {
                    resp.text = result["text"].get<std::string>();
                } else if (result.contains("error")) {
                    resp.type = "error";
                    resp.message = result["error"]["message"].get<std::string>();
                }
            } catch (const std::exception& e) {
                resp.type = "error";
                resp.message = std::string("Final transcription failed: ") + e.what();
            }

            ws->send(resp.to_json().dump(), uWS::OpCode::TEXT);

            // Clean up temp file
            std::filesystem::remove(temp_path);
        } else {
            // No audio to transcribe, just send completion
            AudioStreamResponse resp;
            resp.type = "final";
            resp.text = "";
            resp.is_final = true;
            ws->send(resp.to_json().dump(), uWS::OpCode::TEXT);
        }

        // Clear buffer
        conn->audio_buffer.reset();
        std::cout << "[WebSocket] Stopped streaming" << std::endl;
    }

    template<bool SSL>
    void send_error(uWS::WebSocket<SSL, true, ConnectionState>* ws,
                   const std::string& message) {
        AudioStreamResponse resp;
        resp.type = "error";
        resp.message = message;
        resp.is_final = false;
        ws->send(resp.to_json().dump(), uWS::OpCode::TEXT);
    }

    Router& router_;
    int port_;
    std::atomic<bool> running_{false};
    us_listen_socket_t* listen_socket_ = nullptr;
    uWS::Loop* loop_ = nullptr;

    // Connection management
    std::atomic<uint64_t> next_connection_id_{1};
    std::mutex connections_mutex_;
    std::unordered_map<uint64_t, std::unique_ptr<ConnectionData>> connections_;
};

// WebSocketHandler implementation

WebSocketHandler::WebSocketHandler(Router& router, int port)
    : router_(router), port_(port), impl_(std::make_unique<Impl>(router, port)) {
}

WebSocketHandler::~WebSocketHandler() {
    stop();
}

bool WebSocketHandler::start() {
    if (running_.load()) return true;

    // Start WebSocket server in background thread
    server_thread_ = std::thread([this]() {
        if (impl_->start()) {
            running_.store(true);
        }
    });

    // Wait a bit for server to start
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    return impl_->is_running();
}

void WebSocketHandler::stop() {
    if (!running_.load()) return;

    impl_->stop();
    running_.store(false);

    if (server_thread_.joinable()) {
        server_thread_.join();
    }
}

void WebSocketHandler::set_transcription_callback(TranscriptionCallback callback) {
    // TODO: Store callback for async transcription results
}

} // namespace lemon
