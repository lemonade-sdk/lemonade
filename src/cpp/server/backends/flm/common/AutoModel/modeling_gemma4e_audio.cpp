/// \file modeling_Qwen3VL_image.cpp
/// \brief Gemma4e image processing implementation
/// \author FastFlowLM Team
/// \date 2025-09-01
/// \version 0.9.24
/// \note This is a source file for the Gemma4e image processing functionality

#include "AutoModel/modeling_gemma4e.hpp"
#include "audio_process_utils/audioproc.hpp"
#include "base64.hpp"
#include <utility>
#include <cmath>
#include <algorithm>
#include <numeric>
audio_data_t Gemma4e::load_audio_base64(const std::string &base64_str, int resample_rate, MonoDownmixMode downmix) {
    audio_data_t empty_result;
    audio_data_t result;
    // Decode base64 to raw bytes
    std::string audio_bytes = base64::from_base64(base64_str);
    if (!audio_reader_.load_audio_from_memory(reinterpret_cast<const uint8_t*>(audio_bytes.data()), audio_bytes.size(), result, resample_rate, downmix)) {
        std::cerr << "Failed to load audio from base64 string" << std::endl;
        exit(-1);
        //return empty_result;
    }
    return result;
}




audio_data_t Gemma4e::load_audio(const std::string &filename, int resample_rate, MonoDownmixMode downmix) {
    audio_data_t empty_result;
    audio_data_t result;

    if (!audio_reader_.load_audio(filename, result, resample_rate, downmix)) {
        std::cerr << "Failed to load audio: " << filename << std::endl;
        exit(-1);
        //return empty_result;
    }
    return result;
}

std::vector<audio_data_t> Gemma4e::clip_audio_length(audio_data_t& audio, double max_duration_second) {
    std::vector<audio_data_t> audio_chunks;
    size_t max_frames = static_cast<size_t>(max_duration_second * audio.sample_rate);

    size_t total_frames = audio.num_frames;
    size_t total_samples = audio.num_samples;
    size_t chunk_start_frame = 0;

    while (chunk_start_frame < total_frames) {
        size_t chunk_end_frame = std::min(chunk_start_frame + max_frames, total_frames);
        size_t chunk_start_sample = chunk_start_frame * audio.channels;
        size_t chunk_end_sample = chunk_end_frame * audio.channels;

        audio_data_t chunk;
        chunk.sample_rate = audio.sample_rate;
        chunk.channels = audio.channels;
        chunk.num_frames = chunk_end_frame - chunk_start_frame;
        chunk.num_samples = chunk.num_frames * audio.channels;
        chunk.duration_seconds = static_cast<double>(chunk.num_frames) / audio.sample_rate;
        chunk.samples.assign(audio.samples.begin() + chunk_start_sample, audio.samples.begin() + chunk_end_sample);

        audio_chunks.push_back(std::move(chunk));

        chunk_start_frame = chunk_end_frame;
    }

    return audio_chunks;
}


void Gemma4e::extract_spectrogram(std::vector<audio_data_t>& audio_inputs, gemma4e_audio_payload_t& audio_payload) {

    audio_payload.num_audios = static_cast<unsigned int>(audio_inputs.size());
    audio_payload.mel_spectrograms.resize(audio_payload.num_audios);
    audio_payload.mel_spectrogram_frames_per_audio.resize(audio_payload.num_audios);
    audio_payload.mel_spectrogram_bins_per_audio.resize(audio_payload.num_audios);

    // ------- Config (matches Python __init__ defaults) -------
    constexpr float frame_length_ms   = 20.0f;
    constexpr float hop_length_ms     = 10.0f;
    constexpr float min_frequency     = 0.0f;
    constexpr float max_frequency     = 8000.0f;
    constexpr float mel_floor         = 1e-3f;
    constexpr int   feature_size      = 128;     // num_mel_filters
    constexpr float dither            = 0.0f;
    constexpr float input_scale_factor = 1.0f;
    constexpr float preemphasis       = 0.0f;
    constexpr bool  preemphasis_htk_flavor = true;
    constexpr bool  fft_overdrive     = false;

    for (unsigned int audio_idx = 0; audio_idx < audio_payload.num_audios; audio_idx++) {
    audio_data_t& audio_input = audio_inputs[audio_idx];

    const int sampling_rate = audio_input.sample_rate;

    // frame_length = int(round(sampling_rate * frame_length_ms / 1000.0))
    const int frame_length = static_cast<int>(std::round(sampling_rate * frame_length_ms / 1000.0f));
    // hop_length = int(round(sampling_rate * hop_length_ms / 1000.0))
    const int hop_length   = static_cast<int>(std::round(sampling_rate * hop_length_ms / 1000.0f));

    // fft_length = 2 ** ceil(log2(frame_length))
    int fft_length = 1;
    while (fft_length < frame_length) fft_length <<= 1;
    if (fft_overdrive) fft_length *= 2;

    const int num_frequency_bins = fft_length / 2 + 1;

    // ------- self.window = window_function(frame_length).astype(np.float32) -------
    // periodic Hann window, matching Python: window_function(frame_length)
    // Python default: name="hann", periodic=True
    std::vector<float> window = audioproc::window_function_optimized(frame_length, "hann", /*periodic=*/true);

    // ------- self.mel_filters = mel_filter_bank(...) -------
    std::vector<float> mel_filters = audioproc::mel_filter_bank_optimized(
        num_frequency_bins, feature_size,
        min_frequency, max_frequency,
        sampling_rate, /*apply_slaney_norm=*/false);

    // ------- waveform = audio_input.samples (mono, 1D) -------
    // The Python code works on [B, T]. We handle B=1 (single waveform).
    const float* waveform_ptr = audio_input.samples.data();
    const int original_length = static_cast<int>(audio_input.num_frames);

    // ------- Semicausal time padding: prepend frame_length // 2 zeros -------
    // waveform = np.pad(waveform, ((0,0), (pad_left, 0)), mode="constant")
    const int pad_left = frame_length / 2;
    const int padded_length = original_length + pad_left;
    std::vector<float> waveform(padded_length, 0.0f);
    std::memcpy(waveform.data() + pad_left, waveform_ptr, original_length * sizeof(float));

    // ------- _unfold(waveform, dimension=-1, size=frame_length+1, step=hop_length) -------
    // Output: [num_frames_out, frame_size_for_unfold]
    const int frame_size_for_unfold = frame_length + 1;
    const int num_frames_out = (padded_length - frame_size_for_unfold) / hop_length + 1;

    if (num_frames_out <= 0) continue;

    // ------- Preemphasis == 0.0: frames = frames_to_process[..., :-1] -------
    // Each frame: waveform[i*hop : i*hop + frame_length]  (drop last sample of unfold)
    // Then multiply by window: frames[n] = waveform_frame[n] * window[n]
    //
    // Fused unfold + drop-last + window multiply (AVX512 when available).
    std::vector<float> windowed_frames(num_frames_out * frame_length);
    audioproc::apply_window_frames_optimized(
        waveform.data(),
        window.data(),
        windowed_frames.data(),
        num_frames_out,
        frame_length,
        hop_length);

    // ------- stft = np.fft.rfft(frames, n=fft_length, axis=-1) -------
    // ------- magnitude_spec = np.abs(stft)                     -------
    // rfft_magnitude_batch does both: rfft + abs
    std::vector<float> magnitude_spec(num_frames_out * num_frequency_bins);
    audioproc::rfft_magnitude_batch_optimized(
        windowed_frames.data(),
        magnitude_spec.data(),
        num_frames_out,
        frame_length,
        fft_length);

    // ------- mel_spec = np.matmul(magnitude_spec, self.mel_filters) -------
    // magnitude_spec: [num_frames_out x num_frequency_bins]
    // mel_filters:    [num_frequency_bins x feature_size]
    // out:            [num_frames_out x feature_size]
    std::vector<float> mel_spec(num_frames_out * feature_size);
    audioproc::mel_spectrogram_optimized(
        magnitude_spec.data(),
        mel_filters.data(),
        mel_spec.data(),
        num_frames_out,
        num_frequency_bins,
        feature_size);

    // ------- log_mel_spec = np.log(mel_spec + self.mel_floor) -------
    std::vector<float> log_mel_spec(num_frames_out * feature_size);
    audioproc::log_mel_floor_optimized(
        mel_spec.data(),
        log_mel_spec.data(),
        num_frames_out * feature_size,
        mel_floor);

    // ------- per_bin_mean / per_bin_stddev normalization (skipped if None) -------
    // per_bin_mean and per_bin_stddev are None by default, skip for now.

    // ------- Store results into audio_payload (float -> bf16) -------
    const int total_bins = num_frames_out * feature_size;
    std::vector<bf16> log_mel_spec_bf16(total_bins);
    for (int i = 0; i < total_bins; i++) {
        log_mel_spec_bf16[i] = static_cast<bf16>(log_mel_spec[i]);
    }
    audio_payload.mel_spectrograms[audio_idx] = std::move(log_mel_spec_bf16);
    audio_payload.mel_spectrogram_frames_per_audio[audio_idx] = num_frames_out;
    audio_payload.mel_spectrogram_bins_per_audio[audio_idx] = feature_size;

    } // end for each audio
}
