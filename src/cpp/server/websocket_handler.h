// Copyright (c) 2025 AMD
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <string>
#include <functional>
#include <memory>
#include <thread>
#include <atomic>
#include <nlohmann/json.hpp>

namespace lemon {

// Forward declarations
class Router;

/**
 * @brief Audio streaming message types (client -> server)
 */
struct AudioStreamMessage {
    std::string type;       // "start", "audio_chunk", "stop"
    std::string model;      // Model name (for "start" message)
    std::string language;   // Optional language code
    std::string data;       // Base64 encoded audio data (for "audio_chunk")
    int sample_rate = 16000; // Audio sample rate in Hz

    static AudioStreamMessage from_json(const nlohmann::json& j);
};

/**
 * @brief Audio streaming response types (server -> client)
 */
struct AudioStreamResponse {
    std::string type;       // "partial", "final", "error", "ready"
    std::string text;       // Transcription text
    bool is_final = false;  // True if this is the final transcription
    std::string message;    // Error message (for "error" type)
    double timestamp = 0.0; // Timestamp in seconds

    nlohmann::json to_json() const;
};

/**
 * @brief WebSocket handler for real-time audio streaming transcription
 *
 * Handles WebSocket connections on ws://localhost:<port>/api/v1/audio/stream
 * Protocol:
 *   1. Client connects via WebSocket
 *   2. Client sends {"type": "start", "model": "Whisper-Small", "language": "en"}
 *   3. Server responds {"type": "ready"}
 *   4. Client sends audio chunks: {"type": "audio_chunk", "data": "<base64>", "sample_rate": 16000}
 *   5. Server sends partial results: {"type": "partial", "text": "...", "is_final": false}
 *   6. Client sends {"type": "stop"} when done
 *   7. Server sends final result: {"type": "final", "text": "...", "is_final": true}
 */
class WebSocketHandler {
public:
    /**
     * @brief Construct WebSocket handler
     * @param router Reference to the router for audio transcription
     * @param port Port to listen on (0 = auto-select)
     */
    explicit WebSocketHandler(Router& router, int port = 0);

    ~WebSocketHandler();

    // Non-copyable
    WebSocketHandler(const WebSocketHandler&) = delete;
    WebSocketHandler& operator=(const WebSocketHandler&) = delete;

    /**
     * @brief Start the WebSocket server in a background thread
     * @return True if server started successfully
     */
    bool start();

    /**
     * @brief Stop the WebSocket server
     */
    void stop();

    /**
     * @brief Check if server is running
     */
    bool is_running() const { return running_.load(); }

    /**
     * @brief Get the port the server is listening on
     */
    int get_port() const { return port_; }

    /**
     * @brief Set callback for when transcription results are ready
     */
    using TranscriptionCallback = std::function<void(const AudioStreamResponse&)>;
    void set_transcription_callback(TranscriptionCallback callback);

private:
    // Internal implementation (pimpl idiom to hide uWebSockets details)
    class Impl;
    std::unique_ptr<Impl> impl_;

    Router& router_;
    int port_;
    std::atomic<bool> running_{false};
    std::thread server_thread_;
};

} // namespace lemon
