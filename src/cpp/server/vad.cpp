#include "lemon/vad.h"
#include <cmath>
#include <chrono>
#include <algorithm>

namespace lemon {

SimpleVAD::SimpleVAD()
    : config_() {
}

SimpleVAD::SimpleVAD(const Config& config)
    : config_(config) {
}

float SimpleVAD::calculate_rms(const std::vector<float>& audio) {
    if (audio.empty()) {
        return 0.0f;
    }

    float sum_squares = 0.0f;
    for (float sample : audio) {
        sum_squares += sample * sample;
    }
    return std::sqrt(sum_squares / static_cast<float>(audio.size()));
}

int64_t SimpleVAD::current_time_ms() {
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
}

SimpleVAD::Event SimpleVAD::process(const std::vector<float>& audio, int sample_rate) {
    if (audio.empty()) {
        return Event::None;
    }

    // Calculate RMS energy of this chunk
    float rms = calculate_rms(audio);
    bool is_voice = rms > config_.energy_threshold;

    // Calculate frame duration in milliseconds
    float frame_duration_ms = static_cast<float>(audio.size()) * 1000.0f / static_cast<float>(sample_rate);

    Event result = Event::None;

    if (!speech_active_) {
        // --- Not currently in speech ---
        if (is_voice) {
            onset_counter_++;
            speech_frames_++;

            // Require onset_frames consecutive voice frames to confirm speech start
            float speech_duration_ms = speech_frames_ * frame_duration_ms;
            if (onset_counter_ >= config_.onset_frames && speech_duration_ms >= config_.min_speech_ms) {
                speech_active_ = true;
                hangover_counter_ = config_.hangover_frames;
                speech_start_ms_ = current_time_ms() - static_cast<int64_t>(speech_duration_ms);
                result = Event::SpeechStart;
            }
        } else {
            // Silence frame resets partial onset
            onset_counter_ = 0;
            speech_frames_ = 0;
            silence_frames_ = 0;
        }
    } else {
        // --- Currently in speech ---
        if (is_voice) {
            // Voice frame: reset hangover and silence counters
            hangover_counter_ = config_.hangover_frames;
            silence_frames_ = 0;
            speech_frames_++;
        } else {
            // Silence frame during speech
            if (hangover_counter_ > 0) {
                // Still in hangover period — remain in speech
                hangover_counter_--;
            } else {
                // Hangover exhausted, start counting silence
                silence_frames_++;

                float silence_duration_ms = silence_frames_ * frame_duration_ms;
                if (silence_duration_ms >= config_.min_silence_ms) {
                    speech_active_ = false;
                    speech_end_ms_ = current_time_ms();
                    onset_counter_ = 0;
                    speech_frames_ = 0;
                    silence_frames_ = 0;
                    result = Event::SpeechEnd;
                }
            }
        }
    }

    return result;
}

void SimpleVAD::reset() {
    speech_active_ = false;
    speech_start_ms_ = 0;
    speech_end_ms_ = 0;
    speech_frames_ = 0;
    silence_frames_ = 0;
    onset_counter_ = 0;
    hangover_counter_ = 0;
}

} // namespace lemon
