#pragma once

#include <vector>
#include <chrono>

namespace lemon {

/**
 * Simple energy-based Voice Activity Detection.
 * Detects speech start/end events based on audio energy levels.
 */
class SimpleVAD {
public:
    struct Config {
        float energy_threshold = 0.01f;     // RMS threshold for speech detection
        float freq_threshold = 100.0f;      // Minimum frequency for speech (unused for now)
        int min_speech_ms = 250;            // Minimum speech duration to trigger
        int min_silence_ms = 800;           // Silence duration to end speech (longer = bigger chunks for Whisper)
        int sample_rate = 16000;            // Audio sample rate
        int onset_frames = 2;              // Consecutive voice frames required to confirm speech start
        int hangover_frames = 6;           // Extra frames (~510ms) to continue after silence before ending speech
    };

    enum class Event {
        None,           // No event
        SpeechStart,    // Speech started
        SpeechEnd       // Speech ended (trigger transcription)
    };

    SimpleVAD();
    explicit SimpleVAD(const Config& config);
    ~SimpleVAD() = default;

    /**
     * Process an audio chunk and detect speech events.
     * @param audio Float32 audio samples normalized to [-1.0, 1.0]
     * @param sample_rate Sample rate of the audio (should match config)
     * @return Event type if a speech boundary was detected
     */
    Event process(const std::vector<float>& audio, int sample_rate);

    /**
     * Check if speech is currently active.
     */
    bool is_speech_active() const { return speech_active_; }

    /**
     * Get the timestamp when current speech started (in ms since epoch).
     */
    int64_t speech_start_ms() const { return speech_start_ms_; }

    /**
     * Get the timestamp when speech last ended (in ms since epoch).
     */
    int64_t speech_end_ms() const { return speech_end_ms_; }

    /**
     * Reset the VAD state.
     */
    void reset();

    /**
     * Update configuration.
     */
    void set_config(const Config& config) { config_ = config; }

private:
    Config config_;
    bool speech_active_ = false;
    int64_t speech_start_ms_ = 0;
    int64_t speech_end_ms_ = 0;
    int speech_frames_ = 0;      // Consecutive frames with speech
    int silence_frames_ = 0;     // Consecutive frames without speech
    int onset_counter_ = 0;      // Consecutive voice frames during onset confirmation
    int hangover_counter_ = 0;   // Remaining hangover frames before speech end

    // Calculate RMS energy of audio chunk
    static float calculate_rms(const std::vector<float>& audio);

    // Get current time in milliseconds
    static int64_t current_time_ms();
};

} // namespace lemon
