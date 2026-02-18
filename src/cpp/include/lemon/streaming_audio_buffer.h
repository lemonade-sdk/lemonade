#pragma once

#include <vector>
#include <string>
#include <mutex>
#include <cstdint>

namespace lemon {

/**
 * Thread-safe audio buffer for streaming transcription.
 * Accumulates PCM audio chunks and exports as WAV for whisper.cpp.
 */
class StreamingAudioBuffer {
public:
    static constexpr int SAMPLE_RATE = 16000;  // Whisper native rate
    static constexpr int CHANNELS = 1;          // Mono
    static constexpr int BITS_PER_SAMPLE = 16;  // PCM16

    StreamingAudioBuffer() = default;
    ~StreamingAudioBuffer() = default;

    // Non-copyable
    StreamingAudioBuffer(const StreamingAudioBuffer&) = delete;
    StreamingAudioBuffer& operator=(const StreamingAudioBuffer&) = delete;

    /**
     * Append base64-encoded PCM16 audio data to the buffer.
     * @param base64_audio Base64-encoded PCM16 mono 16kHz audio
     */
    void append(const std::string& base64_audio);

    /**
     * Append raw PCM16 audio samples directly.
     * @param samples Raw int16 samples at 16kHz mono
     */
    void append_raw(const std::vector<int16_t>& samples);

    /**
     * Get the accumulated audio as a WAV file in memory.
     * @return WAV file bytes ready to write to disk or send to whisper
     */
    std::vector<uint8_t> get_wav() const;

    /**
     * Get the accumulated audio as a WAV file, padded with silence to a minimum duration.
     * Prevents Whisper hallucinations on very short audio clips.
     * @param min_duration_ms Minimum audio duration in milliseconds (default 1250ms)
     * @return WAV file bytes with silence padding if needed
     */
    std::vector<uint8_t> get_wav_padded(int min_duration_ms = 1250) const;

    /**
     * Get the accumulated audio as float32 samples (for VAD processing).
     * @return Float32 samples normalized to [-1.0, 1.0]
     */
    std::vector<float> get_samples() const;

    /**
     * Get the most recent N milliseconds of audio as float32 samples.
     * @param ms Number of milliseconds of audio to retrieve
     * @return Float32 samples normalized to [-1.0, 1.0]
     */
    std::vector<float> get_recent_samples(int ms) const;

    /**
     * Clear the audio buffer.
     */
    void clear();

    /**
     * Get the duration of accumulated audio in milliseconds.
     */
    int duration_ms() const;

    /**
     * Get the number of samples in the buffer.
     */
    size_t sample_count() const;

    /**
     * Check if the buffer is empty.
     */
    bool empty() const;

private:
    std::vector<int16_t> samples_;
    mutable std::mutex mutex_;

    // Helper to build WAV from samples (no locking — caller must hold mutex_)
    static std::vector<uint8_t> build_wav(const std::vector<int16_t>& samples);
};

} // namespace lemon
