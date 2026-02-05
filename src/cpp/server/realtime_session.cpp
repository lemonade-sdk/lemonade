#include "lemon/realtime_session.h"
#include "lemon/router.h"
#include <random>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <fstream>
#include <iostream>
#include <cmath>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace lemon {

RealtimeSessionManager::RealtimeSessionManager(Router* router)
    : router_(router) {
}

RealtimeSessionManager::~RealtimeSessionManager() {
    // Wait for pending transcriptions to complete
    {
        std::lock_guard<std::mutex> lock(transcriptions_mutex_);
        for (auto& f : pending_transcriptions_) {
            if (f.valid()) f.wait();
        }
        pending_transcriptions_.clear();
    }

    // Close all sessions
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    sessions_.clear();
}

std::string RealtimeSessionManager::generate_session_id() {
    // Generate a random session ID
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 15);

    const char* hex_chars = "0123456789abcdef";
    std::string id = "sess_";

    for (int i = 0; i < 24; i++) {
        id += hex_chars[dis(gen)];
    }

    return id;
}

std::string RealtimeSessionManager::create_session(
    std::function<void(const json&)> send_callback,
    const json& config
) {
    std::string session_id = generate_session_id();

    auto session = std::make_shared<RealtimeSession>(session_id);
    session->send_message = std::move(send_callback);

    // Apply initial configuration
    if (config.contains("model")) {
        session->model = config["model"].get<std::string>();
    }

    // Configure VAD if specified
    if (config.contains("turn_detection")) {
        const auto& td = config["turn_detection"];
        SimpleVAD::Config vad_config;

        if (td.contains("threshold")) {
            vad_config.energy_threshold = td["threshold"].get<float>();
        }
        if (td.contains("silence_duration_ms")) {
            vad_config.min_silence_ms = td["silence_duration_ms"].get<int>();
        }
        if (td.contains("prefix_padding_ms")) {
            vad_config.min_speech_ms = td["prefix_padding_ms"].get<int>();
        }

        session->vad.set_config(vad_config);
    }

    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        sessions_[session_id] = std::move(session);
    }

    // Send session created message (OpenAI-compatible)
    json created_msg = {
        {"type", "session.created"},
        {"session", {
            {"id", session_id}
        }}
    };

    // Get the session and send
    auto sess = get_session(session_id);
    if (sess && sess->send_message) {
        sess->send_message(created_msg);
    }

    return session_id;
}

void RealtimeSessionManager::update_session(const std::string& session_id, const json& config) {
    auto session = get_session(session_id);
    if (!session) {
        return;
    }

    if (config.contains("model")) {
        session->model = config["model"].get<std::string>();
    }

    if (config.contains("turn_detection")) {
        const auto& td = config["turn_detection"];
        SimpleVAD::Config vad_config;

        if (td.contains("threshold")) {
            vad_config.energy_threshold = td["threshold"].get<float>();
        }
        if (td.contains("silence_duration_ms")) {
            vad_config.min_silence_ms = td["silence_duration_ms"].get<int>();
        }
        if (td.contains("prefix_padding_ms")) {
            vad_config.min_speech_ms = td["prefix_padding_ms"].get<int>();
        }

        session->vad.set_config(vad_config);
    }

    // Send session updated message (OpenAI-compatible)
    if (session->send_message) {
        json updated_msg = {
            {"type", "session.updated"},
            {"session", {
                {"id", session_id},
                {"model", session->model}
            }}
        };
        session->send_message(updated_msg);
    }
}

void RealtimeSessionManager::append_audio(const std::string& session_id, const std::string& base64_audio) {
    auto session = get_session(session_id);
    if (!session || !session->session_active) {
        return;
    }

    // Append to buffer
    session->audio_buffer.append(base64_audio);

    // Log buffer growth periodically (every ~5 seconds at 256ms chunks ≈ every 20 chunks)
    static int chunk_count = 0;
    if (++chunk_count % 20 == 1) {
        std::cout << "[RealtimeSession] Audio buffer: " << session->audio_buffer.duration_ms()
                  << "ms (" << session->audio_buffer.sample_count() << " samples)" << std::endl;
    }

    // Process VAD with recent audio
    process_vad(session);
}

void RealtimeSessionManager::process_vad(std::shared_ptr<RealtimeSession> session) {
    // Get recent audio for VAD processing (last 100ms)
    auto recent_audio = session->audio_buffer.get_recent_samples(100);
    if (recent_audio.empty()) {
        return;
    }

    // Log RMS periodically for threshold tuning
    {
        float sum_sq = 0.0f;
        for (float s : recent_audio) sum_sq += s * s;
        float rms = std::sqrt(sum_sq / static_cast<float>(recent_audio.size()));
        static int vad_log_count = 0;
        if (++vad_log_count % 20 == 1) {
            std::cout << "[RealtimeSession] VAD: RMS=" << rms
                      << " speech_active=" << session->vad.is_speech_active() << std::endl;
        }
    }

    SimpleVAD::Event event = session->vad.process(recent_audio, StreamingAudioBuffer::SAMPLE_RATE);

    switch (event) {
        case SimpleVAD::Event::SpeechStart: {
            std::cout << "[RealtimeSession] VAD: SpeechStart detected" << std::endl;
            session->audio_start_ms = session->vad.speech_start_ms();
            session->last_interim_transcription_ms = 0;  // Reset interim tracking for new utterance

            if (session->send_message) {
                json msg = {
                    {"type", "input_audio_buffer.speech_started"},
                    {"audio_start_ms", session->audio_start_ms}
                };
                session->send_message(msg);
            }
            break;
        }

        case SimpleVAD::Event::SpeechEnd: {
            std::cout << "[RealtimeSession] VAD: SpeechEnd detected, triggering transcription" << std::endl;
            int64_t audio_end_ms = session->vad.speech_end_ms();

            if (session->send_message) {
                json msg = {
                    {"type", "input_audio_buffer.speech_stopped"},
                    {"audio_end_ms", audio_end_ms}
                };
                session->send_message(msg);
            }

            // Trigger final transcription (clears buffer)
            transcribe_and_send(session);
            break;
        }

        case SimpleVAD::Event::None:
        default:
            // Speech is ongoing — check if we should fire an interim transcription
            if (session->vad.is_speech_active()) {
                maybe_interim_transcribe(session);
            }
            break;
    }
}

void RealtimeSessionManager::maybe_interim_transcribe(std::shared_ptr<RealtimeSession> session) {
    if (!session || session->audio_buffer.empty()) return;

    int buffer_ms = session->audio_buffer.duration_ms();

    // Determine how much new audio has arrived since the last interim transcription.
    // On the first interim of an utterance last_interim_transcription_ms is 0, so we
    // compare against the raw buffer duration.
    int since_last = (session->last_interim_transcription_ms == 0)
        ? buffer_ms
        : buffer_ms - session->last_interim_transcription_ms;

    if (since_last >= INTERIM_TRANSCRIPTION_CHUNK_MS) {
        transcribe_interim(session);
    }
}

void RealtimeSessionManager::transcribe_interim(std::shared_ptr<RealtimeSession> session) {
    if (!session || session->audio_buffer.empty()) return;

    // Avoid overlapping interim transcriptions for the same session
    bool expected = false;
    if (!session->interim_in_flight.compare_exchange_strong(expected, true)) {
        return;  // Another interim is already running
    }

    // Snapshot the buffer WITHOUT clearing it
    auto wav_data = session->audio_buffer.get_wav_padded(500);
    std::string model = session->model;
    session->last_interim_transcription_ms = session->audio_buffer.duration_ms();

    std::cout << "[RealtimeSession] Firing interim transcription at "
              << session->last_interim_transcription_ms << "ms" << std::endl;

    auto future = std::async(std::launch::async,
        [this, session, wav_data = std::move(wav_data), model = std::move(model)]() {
            transcribe_wav(session, wav_data, model, /*is_interim=*/true);
            session->interim_in_flight.store(false);
        });

    {
        std::lock_guard<std::mutex> lock(transcriptions_mutex_);
        pending_transcriptions_.erase(
            std::remove_if(pending_transcriptions_.begin(), pending_transcriptions_.end(),
                [](const std::future<void>& f) {
                    return f.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
                }),
            pending_transcriptions_.end()
        );
        pending_transcriptions_.push_back(std::move(future));
    }
}

void RealtimeSessionManager::commit_audio(const std::string& session_id) {
    auto session = get_session(session_id);
    if (!session || session->audio_buffer.empty()) {
        return;
    }

    // Send committed message
    if (session->send_message) {
        json msg = {
            {"type", "input_audio_buffer.committed"}
        };
        session->send_message(msg);
    }

    // Trigger transcription
    transcribe_and_send(session);
}

void RealtimeSessionManager::clear_audio(const std::string& session_id) {
    auto session = get_session(session_id);
    if (!session) {
        return;
    }

    session->audio_buffer.clear();
    session->vad.reset();

    if (session->send_message) {
        json msg = {
            {"type", "input_audio_buffer.cleared"}
        };
        session->send_message(msg);
    }
}

void RealtimeSessionManager::transcribe_and_send(std::shared_ptr<RealtimeSession> session) {
    if (!session || session->audio_buffer.empty()) {
        return;
    }

    // Snapshot WAV data and clear buffer on the callback thread (no data race)
    auto wav_data = session->audio_buffer.get_wav_padded(500);
    std::string model = session->model;
    session->audio_buffer.clear();
    session->vad.reset();
    session->last_interim_transcription_ms = 0;  // Reset for next utterance

    // Dispatch transcription to worker thread so it doesn't block the WebSocket callback
    auto future = std::async(std::launch::async,
        [this, session, wav_data = std::move(wav_data), model = std::move(model)]() {
            transcribe_wav(session, wav_data, model);
        });

    // Track future for clean shutdown
    {
        std::lock_guard<std::mutex> lock(transcriptions_mutex_);
        // Remove completed futures
        pending_transcriptions_.erase(
            std::remove_if(pending_transcriptions_.begin(), pending_transcriptions_.end(),
                [](const std::future<void>& f) {
                    return f.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
                }),
            pending_transcriptions_.end()
        );
        pending_transcriptions_.push_back(std::move(future));
    }
}

void RealtimeSessionManager::transcribe_wav(
    std::shared_ptr<RealtimeSession> session,
    std::vector<uint8_t> wav_data, std::string model,
    bool is_interim) {
    try {
        // Convert WAV bytes to a string for the router (expects file_data as string)
        std::string file_data(reinterpret_cast<const char*>(wav_data.data()), wav_data.size());

        // Build transcription request
        json request = {
            {"model", model},
            {"file_data", file_data},
            {"filename", "realtime_audio.wav"}
        };

        // Call router for transcription
        const char* tag = is_interim ? "interim" : "final";
        std::cout << "[RealtimeSession] Calling Whisper " << tag << " transcription ("
                  << wav_data.size() << " bytes)..." << std::endl;
        json response = router_->audio_transcriptions(request);
        std::cout << "[RealtimeSession] Whisper " << tag << " response: " << response.dump() << std::endl;

        // Send transcription result if session is still active
        if (session->send_message && session->session_active.load()) {
            std::string transcript;
            if (response.contains("text")) {
                transcript = response["text"].get<std::string>();
            }

            std::cout << "[RealtimeSession] Sending " << tag << " transcript to client: \""
                      << transcript << "\"" << std::endl;

            if (is_interim) {
                // Interim/partial result — client should treat as replaceable
                json msg = {
                    {"type", "conversation.item.input_audio_transcription.delta"},
                    {"delta", transcript}
                };
                session->send_message(msg);
            } else {
                // Final result — speech segment is complete
                json msg = {
                    {"type", "conversation.item.input_audio_transcription.completed"},
                    {"transcript", transcript}
                };
                session->send_message(msg);
            }
        }

    } catch (const std::exception& e) {
        std::cerr << "[RealtimeSession] Transcription error: " << e.what() << std::endl;

        if (session->send_message && session->session_active.load()) {
            json error_msg = {
                {"type", "error"},
                {"error", {
                    {"message", std::string("Transcription failed: ") + e.what()},
                    {"type", "transcription_error"}
                }}
            };
            session->send_message(error_msg);
        }
    }
}

void RealtimeSessionManager::close_session(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);

    auto it = sessions_.find(session_id);
    if (it != sessions_.end()) {
        it->second->session_active = false;
        sessions_.erase(it);
    }
}

bool RealtimeSessionManager::session_exists(const std::string& session_id) const {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    return sessions_.find(session_id) != sessions_.end();
}

std::shared_ptr<RealtimeSession> RealtimeSessionManager::get_session(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);

    auto it = sessions_.find(session_id);
    if (it != sessions_.end()) {
        return it->second;
    }
    return nullptr;
}

} // namespace lemon
