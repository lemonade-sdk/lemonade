#include "lemon/streaming_audio_buffer.h"
#include <cstring>
#include <stdexcept>
#include <algorithm>
#include <iostream>

namespace lemon {

// Base64 decoding table
static const unsigned char base64_decode_table[256] = {
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 62, 64, 64, 64, 63,
    52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 64, 64, 64, 64, 64, 64,
    64,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
    15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 64, 64, 64, 64, 64,
    64, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
    41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64
};

std::vector<uint8_t> StreamingAudioBuffer::base64_decode(const std::string& encoded) {
    if (encoded.empty()) {
        return {};
    }

    size_t in_len = encoded.size();
    // Skip trailing '=' padding
    while (in_len > 0 && encoded[in_len - 1] == '=') {
        in_len--;
    }

    size_t out_len = in_len * 3 / 4;
    std::vector<uint8_t> decoded(out_len);

    size_t i = 0, j = 0;
    uint32_t sextet_a, sextet_b, sextet_c, sextet_d, triple;

    while (i < in_len) {
        sextet_a = encoded[i] == '=' ? 0 & i++ : base64_decode_table[static_cast<unsigned char>(encoded[i++])];
        sextet_b = encoded[i] == '=' ? 0 & i++ : base64_decode_table[static_cast<unsigned char>(encoded[i++])];
        sextet_c = encoded[i] == '=' ? 0 & i++ : base64_decode_table[static_cast<unsigned char>(encoded[i++])];
        sextet_d = encoded[i] == '=' ? 0 & i++ : base64_decode_table[static_cast<unsigned char>(encoded[i++])];

        triple = (sextet_a << 18) + (sextet_b << 12) + (sextet_c << 6) + sextet_d;

        if (j < out_len) decoded[j++] = (triple >> 16) & 0xFF;
        if (j < out_len) decoded[j++] = (triple >> 8) & 0xFF;
        if (j < out_len) decoded[j++] = triple & 0xFF;
    }

    decoded.resize(j);
    return decoded;
}

void StreamingAudioBuffer::append(const std::string& base64_audio) {
    if (base64_audio.empty()) {
        return;
    }

    // Decode base64 to raw bytes
    std::vector<uint8_t> raw_bytes = base64_decode(base64_audio);

    // Convert bytes to int16 samples (little-endian)
    size_t num_samples = raw_bytes.size() / 2;
    std::vector<int16_t> new_samples(num_samples);

    for (size_t i = 0; i < num_samples; i++) {
        new_samples[i] = static_cast<int16_t>(
            raw_bytes[i * 2] | (raw_bytes[i * 2 + 1] << 8)
        );
    }

    // Diagnostic: log first chunk's data to verify decode pipeline
    static bool logged_first = false;
    if (!logged_first && num_samples > 0) {
        logged_first = true;
        std::cout << "[AudioBuffer] base64 length=" << base64_audio.size()
                  << " decoded bytes=" << raw_bytes.size()
                  << " samples=" << num_samples << std::endl;
        std::cout << "[AudioBuffer] first 8 raw bytes:";
        for (size_t i = 0; i < std::min(raw_bytes.size(), size_t(8)); i++) {
            std::cout << " " << static_cast<int>(raw_bytes[i]);
        }
        std::cout << std::endl;
        std::cout << "[AudioBuffer] first 4 int16 samples:";
        for (size_t i = 0; i < std::min(num_samples, size_t(4)); i++) {
            std::cout << " " << new_samples[i];
        }
        std::cout << std::endl;
        std::cout << "[AudioBuffer] base64 prefix: " << base64_audio.substr(0, 40) << std::endl;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    samples_.insert(samples_.end(), new_samples.begin(), new_samples.end());
}

void StreamingAudioBuffer::append_raw(const std::vector<int16_t>& samples) {
    if (samples.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    samples_.insert(samples_.end(), samples.begin(), samples.end());
}

std::vector<uint8_t> StreamingAudioBuffer::build_wav(const std::vector<int16_t>& samples) {
    // WAV file header constants
    const uint32_t data_size = static_cast<uint32_t>(samples.size() * sizeof(int16_t));
    const uint32_t file_size = 36 + data_size;
    const uint16_t audio_format = 1;  // PCM
    const uint16_t num_channels = CHANNELS;
    const uint32_t sample_rate_val = SAMPLE_RATE;
    const uint32_t byte_rate = sample_rate_val * num_channels * (BITS_PER_SAMPLE / 8);
    const uint16_t block_align = num_channels * (BITS_PER_SAMPLE / 8);
    const uint16_t bits_per_sample = BITS_PER_SAMPLE;

    std::vector<uint8_t> wav;
    wav.reserve(44 + data_size);

    // Helper to write little-endian values
    auto write_u16 = [&wav](uint16_t val) {
        wav.push_back(val & 0xFF);
        wav.push_back((val >> 8) & 0xFF);
    };
    auto write_u32 = [&wav](uint32_t val) {
        wav.push_back(val & 0xFF);
        wav.push_back((val >> 8) & 0xFF);
        wav.push_back((val >> 16) & 0xFF);
        wav.push_back((val >> 24) & 0xFF);
    };

    // RIFF header
    wav.push_back('R'); wav.push_back('I'); wav.push_back('F'); wav.push_back('F');
    write_u32(file_size);
    wav.push_back('W'); wav.push_back('A'); wav.push_back('V'); wav.push_back('E');

    // fmt chunk
    wav.push_back('f'); wav.push_back('m'); wav.push_back('t'); wav.push_back(' ');
    write_u32(16);  // Subchunk1Size for PCM
    write_u16(audio_format);
    write_u16(num_channels);
    write_u32(sample_rate_val);
    write_u32(byte_rate);
    write_u16(block_align);
    write_u16(bits_per_sample);

    // data chunk
    wav.push_back('d'); wav.push_back('a'); wav.push_back('t'); wav.push_back('a');
    write_u32(data_size);

    // Audio data (already in little-endian int16 format)
    const uint8_t* sample_bytes = reinterpret_cast<const uint8_t*>(samples.data());
    wav.insert(wav.end(), sample_bytes, sample_bytes + data_size);

    return wav;
}

std::vector<uint8_t> StreamingAudioBuffer::get_wav() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return build_wav(samples_);
}

std::vector<uint8_t> StreamingAudioBuffer::get_wav_padded(int min_duration_ms) const {
    std::lock_guard<std::mutex> lock(mutex_);

    size_t min_samples = static_cast<size_t>(min_duration_ms) * SAMPLE_RATE / 1000;

    if (samples_.size() >= min_samples) {
        // No padding needed
        return build_wav(samples_);
    }

    // Copy samples and pad with silence (zeros) at the end
    std::vector<int16_t> padded_samples(samples_);
    padded_samples.resize(min_samples, 0);

    return build_wav(padded_samples);
}

std::vector<float> StreamingAudioBuffer::get_samples() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<float> float_samples(samples_.size());
    for (size_t i = 0; i < samples_.size(); i++) {
        float_samples[i] = samples_[i] / 32768.0f;
    }
    return float_samples;
}

std::vector<float> StreamingAudioBuffer::get_recent_samples(int ms) const {
    std::lock_guard<std::mutex> lock(mutex_);

    size_t num_samples = static_cast<size_t>(ms * SAMPLE_RATE / 1000);
    if (num_samples > samples_.size()) {
        num_samples = samples_.size();
    }

    std::vector<float> float_samples(num_samples);
    size_t start_idx = samples_.size() - num_samples;

    for (size_t i = 0; i < num_samples; i++) {
        float_samples[i] = samples_[start_idx + i] / 32768.0f;
    }
    return float_samples;
}

void StreamingAudioBuffer::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    samples_.clear();
}

int StreamingAudioBuffer::duration_ms() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(samples_.size() * 1000 / SAMPLE_RATE);
}

size_t StreamingAudioBuffer::sample_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return samples_.size();
}

bool StreamingAudioBuffer::empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return samples_.empty();
}

} // namespace lemon
