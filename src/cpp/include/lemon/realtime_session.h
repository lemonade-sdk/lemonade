#pragma once

#include <string>
#include <memory>
#include <unordered_map>
#include <mutex>
#include <functional>
#include <future>
#include <atomic>
#include <nlohmann/json.hpp>
#include "streaming_audio_buffer.h"
#include "vad.h"

namespace lemon {

using json = nlohmann::json;

// Forward declaration
class Router;

/**
 * State for a single realtime transcription session.
 */
struct RealtimeSession {
    std::string session_id;
    std::string model;
    StreamingAudioBuffer audio_buffer;
    SimpleVAD vad;
    std::atomic<bool> session_active{true};

    // Callback to send messages back to the WebSocket client
    std::function<void(const json&)> send_message;

    // Timestamps for audio tracking
    int64_t audio_start_ms = 0;  // Start of current speech segment

    // Interim transcription state
    int64_t last_interim_transcription_ms = 0;  // When we last fired an interim transcription
    std::atomic<bool> interim_in_flight{false};  // Guard against overlapping interim requests

    RealtimeSession(const std::string& id)
        : session_id(id), vad(SimpleVAD::Config{}) {}
};

/**
 * Manages realtime transcription sessions.
 * Handles audio buffering, VAD, and transcription routing.
 */
class RealtimeSessionManager {
public:
    // Minimum audio accumulation before firing an interim transcription (ms).
    // Lower values feel more "real-time" but increase Whisper load.
    static constexpr int INTERIM_TRANSCRIPTION_CHUNK_MS = 1000;

    explicit RealtimeSessionManager(Router* router);
    ~RealtimeSessionManager();

    // Non-copyable
    RealtimeSessionManager(const RealtimeSessionManager&) = delete;
    RealtimeSessionManager& operator=(const RealtimeSessionManager&) = delete;

    /**
     * Create a new transcription session.
     * @param send_callback Function to send messages back to WebSocket client
     * @param config Initial session configuration
     * @return Session ID
     */
    std::string create_session(
        std::function<void(const json&)> send_callback,
        const json& config = json::object()
    );

    /**
     * Update session configuration.
     * @param session_id Session to update
     * @param config New configuration (model, VAD settings, etc.)
     */
    void update_session(const std::string& session_id, const json& config);

    /**
     * Append audio data to a session.
     * @param session_id Session to append to
     * @param base64_audio Base64-encoded PCM16 audio
     */
    void append_audio(const std::string& session_id, const std::string& base64_audio);

    /**
     * Commit the current audio buffer (force transcription).
     * @param session_id Session to commit
     */
    void commit_audio(const std::string& session_id);

    /**
     * Clear the audio buffer without transcribing.
     * @param session_id Session to clear
     */
    void clear_audio(const std::string& session_id);

    /**
     * Close and cleanup a session.
     * @param session_id Session to close
     */
    void close_session(const std::string& session_id);

    /**
     * Check if a session exists.
     */
    bool session_exists(const std::string& session_id) const;

private:
    Router* router_;
    std::unordered_map<std::string, std::shared_ptr<RealtimeSession>> sessions_;
    mutable std::mutex sessions_mutex_;

    // Pending transcription futures for clean shutdown
    std::vector<std::future<void>> pending_transcriptions_;
    std::mutex transcriptions_mutex_;

    // Generate unique session ID
    static std::string generate_session_id();

    // Snapshot audio buffer and dispatch transcription to worker thread
    void transcribe_and_send(std::shared_ptr<RealtimeSession> session);

    // Fire an interim (partial) transcription without clearing the buffer
    void transcribe_interim(std::shared_ptr<RealtimeSession> session);

    // Check whether an interim transcription should fire and trigger it
    void maybe_interim_transcribe(std::shared_ptr<RealtimeSession> session);

    // Run Whisper transcription (executes on worker thread)
    // When is_interim is true the result is sent as a delta event.
    void transcribe_wav(std::shared_ptr<RealtimeSession> session,
                        std::vector<uint8_t> wav_data, std::string model,
                        bool is_interim = false);

    // Process VAD for a session
    void process_vad(std::shared_ptr<RealtimeSession> session);

    // Get session by ID (returns nullptr if not found)
    std::shared_ptr<RealtimeSession> get_session(const std::string& session_id);
};

} // namespace lemon
