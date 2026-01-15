// Copyright (c) 2025 AMD
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <vector>
#include <cstdint>
#include <cstddef>
#include <mutex>

namespace lemon {

/**
 * @brief Buffer for accumulating audio chunks for streaming transcription
 *
 * Accumulates audio data until a threshold is reached, then provides
 * the audio for transcription. Supports multiple audio formats and
 * sample rates.
 */
class AudioBuffer {
public:
    /**
     * @brief Construct audio buffer with specified sample rate
     * @param sample_rate Audio sample rate in Hz (default: 16000)
     * @param threshold_seconds Minimum audio duration before transcription (default: 3.0)
     * @param keep_seconds Audio to keep from previous transcription for context (default: 0.2)
     *                     Following whisper.cpp stream example which uses 200ms default
     */
    explicit AudioBuffer(int sample_rate = 16000, double threshold_seconds = 3.0, double keep_seconds = 0.2)
        : sample_rate_(sample_rate)
        , threshold_seconds_(threshold_seconds)
        , keep_seconds_(keep_seconds)
        , bytes_per_sample_(2)  // 16-bit PCM
    {
        // Pre-allocate for ~30 seconds of audio
        buffer_.reserve(sample_rate * bytes_per_sample_ * 30);
    }

    /**
     * @brief Add audio chunk to buffer
     * @param data Pointer to audio data (expected: 16-bit PCM)
     * @param size Size of data in bytes
     */
    void add_chunk(const uint8_t* data, size_t size) {
        std::lock_guard<std::mutex> lock(mutex_);
        buffer_.insert(buffer_.end(), data, data + size);
    }

    /**
     * @brief Check if buffer has enough audio for transcription
     * @return True if accumulated audio exceeds threshold
     */
    bool has_enough_audio() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return get_duration_locked() >= threshold_seconds_;
    }

    /**
     * @brief Get accumulated duration in seconds
     */
    double get_duration() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return get_duration_locked();
    }

    /**
     * @brief Get audio data for transcription and clear buffer
     * @return Vector containing accumulated audio data
     */
    std::vector<uint8_t> get_audio_for_transcription() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<uint8_t> result = std::move(buffer_);
        buffer_.clear();
        buffer_.reserve(sample_rate_ * bytes_per_sample_ * 30);
        return result;
    }

    /**
     * @brief Get audio data for transcription, keeping overlap for context
     *
     * This implements the sliding window approach used by whisper.cpp's stream example.
     * It clears the buffer but keeps the last keep_seconds_ of audio to provide
     * context for the next transcription, preventing word boundary issues.
     *
     * @return Vector containing accumulated audio data (full buffer before clear)
     */
    std::vector<uint8_t> get_audio_and_keep_overlap() {
        std::lock_guard<std::mutex> lock(mutex_);

        // Return a copy of the full buffer for transcription
        std::vector<uint8_t> result = buffer_;

        // Calculate how many bytes to keep for overlap
        size_t keep_samples = static_cast<size_t>(keep_seconds_ * sample_rate_);
        size_t keep_bytes = keep_samples * bytes_per_sample_;

        if (keep_bytes > 0 && buffer_.size() > keep_bytes) {
            // Keep only the last keep_bytes of audio for context
            std::vector<uint8_t> overlap(buffer_.end() - keep_bytes, buffer_.end());
            buffer_ = std::move(overlap);
        } else if (buffer_.size() <= keep_bytes) {
            // Buffer is smaller than keep amount, keep it all
            // (don't clear anything)
        } else {
            // keep_bytes is 0, clear everything
            buffer_.clear();
            buffer_.reserve(sample_rate_ * bytes_per_sample_ * 30);
        }

        return result;
    }

    /**
     * @brief Get audio data without clearing buffer (for partial transcription)
     * @return Copy of accumulated audio data
     */
    std::vector<uint8_t> peek_audio() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return buffer_;
    }

    /**
     * @brief Clear the buffer
     */
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        buffer_.clear();
    }

    /**
     * @brief Get current buffer size in bytes
     */
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return buffer_.size();
    }

    /**
     * @brief Get sample rate
     */
    int get_sample_rate() const { return sample_rate_; }

    /**
     * @brief Set transcription threshold in seconds
     */
    void set_threshold(double seconds) {
        std::lock_guard<std::mutex> lock(mutex_);
        threshold_seconds_ = seconds;
    }

    /**
     * @brief Set keep overlap in seconds (for sliding window context)
     */
    void set_keep_seconds(double seconds) {
        std::lock_guard<std::mutex> lock(mutex_);
        keep_seconds_ = seconds;
    }

    /**
     * @brief Get keep overlap in seconds
     */
    double get_keep_seconds() const { return keep_seconds_; }

    /**
     * @brief Save audio buffer to WAV file
     * @param filepath Path to save WAV file
     * @return True if saved successfully
     */
    bool save_to_wav(const std::string& filepath) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return save_wav_locked(filepath);
    }

private:
    double get_duration_locked() const {
        size_t num_samples = buffer_.size() / bytes_per_sample_;
        return static_cast<double>(num_samples) / sample_rate_;
    }

    bool save_wav_locked(const std::string& filepath) const {
        // WAV file header
        struct WavHeader {
            char riff[4] = {'R', 'I', 'F', 'F'};
            uint32_t file_size;
            char wave[4] = {'W', 'A', 'V', 'E'};
            char fmt[4] = {'f', 'm', 't', ' '};
            uint32_t fmt_size = 16;
            uint16_t audio_format = 1;  // PCM
            uint16_t num_channels = 1;  // Mono
            uint32_t sample_rate;
            uint32_t byte_rate;
            uint16_t block_align;
            uint16_t bits_per_sample = 16;
            char data[4] = {'d', 'a', 't', 'a'};
            uint32_t data_size;
        };

        WavHeader header;
        header.sample_rate = sample_rate_;
        header.byte_rate = sample_rate_ * bytes_per_sample_;
        header.block_align = bytes_per_sample_;
        header.data_size = static_cast<uint32_t>(buffer_.size());
        header.file_size = header.data_size + sizeof(WavHeader) - 8;

        FILE* fp = fopen(filepath.c_str(), "wb");
        if (!fp) return false;

        fwrite(&header, sizeof(header), 1, fp);
        fwrite(buffer_.data(), 1, buffer_.size(), fp);
        fclose(fp);

        return true;
    }

    int sample_rate_;
    double threshold_seconds_;
    double keep_seconds_;
    int bytes_per_sample_;
    std::vector<uint8_t> buffer_;
    mutable std::mutex mutex_;
};

} // namespace lemon
