#pragma once

#include <vector>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <stdexcept>
#include "fftw3.h"

namespace audioproc {

    // ---------------------------------------------------------------
    //  Mel-scale conversion helpers  (HTK scale: 2595 * log10(1 + f/700))
    // ---------------------------------------------------------------
    inline float hertz_to_mel(float freq) {
        return 2595.0f * std::log10(1.0f + freq / 700.0f);
    }

    inline float mel_to_hertz(float mel) {
        return 700.0f * (std::pow(10.0f, mel / 2595.0f) - 1.0f);
    }

    // Compute real-to-complex FFT (rfft) for a batch of frames.
    // Equivalent to np.fft.rfft(frames, n=fft_length, axis=-1)
    //
    // frames:        input float array, row-major [num_frames * frame_length]
    // out_complex:   output interleaved (re,im) [num_frames * (fft_length/2+1) * 2]
    // num_frames:    number of frames in the batch
    // frame_length:  number of samples per frame (will be zero-padded to fft_length)
    // fft_length:    FFT size (must be >= frame_length)
    void rfft_batch(
        const float* frames,
        float* out_complex,
        int num_frames,
        int frame_length,
        int fft_length);

    // Compute rfft then magnitude |stft| for a batch of frames.
    // Equivalent to np.abs(np.fft.rfft(frames, n=fft_length, axis=-1))
    //
    // out_magnitude: output float array [num_frames * (fft_length/2+1)]
    void rfft_magnitude_batch(
        const float* frames,
        float* out_magnitude,
        int num_frames,
        int frame_length,
        int fft_length);

    // Auto-dispatching optimized versions (AVX512 when available, scalar fallback)
    void rfft_batch_optimized(
        const float* frames,
        float* out_complex,
        int num_frames,
        int frame_length,
        int fft_length);

    void rfft_magnitude_batch_optimized(
        const float* frames,
        float* out_magnitude,
        int num_frames,
        int frame_length,
        int fft_length);

    // ---------------------------------------------------------------
    //  Mel filter bank generation
    //  Equivalent to HuggingFace audio_utils.mel_filter_bank() with
    //  mel_scale="htk", norm=None, triangularize_in_mel_space=False.
    // ---------------------------------------------------------------

    // Generate mel filter bank matrix [num_frequency_bins x num_mel_filters],
    // stored in row-major order.
    //
    // num_frequency_bins:  fft_length / 2 + 1
    // num_mel_filters:     e.g. 128
    // min_frequency:       lowest frequency of interest in Hz (e.g. 0)
    // max_frequency:       highest frequency of interest in Hz (e.g. 8000)
    // sampling_rate:       sample rate of the audio waveform (e.g. 16000)
    // apply_slaney_norm:   if true, apply Slaney area-normalization
    std::vector<float> mel_filter_bank(
        int num_frequency_bins,
        int num_mel_filters,
        float min_frequency,
        float max_frequency,
        int sampling_rate,
        bool apply_slaney_norm = false);

    // Compute mel spectrogram:  out = magnitude_spec @ mel_filters
    // magnitude_spec: [num_frames x num_frequency_bins]  (row-major)
    // mel_filters:    [num_frequency_bins x num_mel_filters]  (row-major)
    // out:            [num_frames x num_mel_filters]  (row-major)
    void mel_spectrogram(
        const float* magnitude_spec,
        const float* mel_filters,
        float* out,
        int num_frames,
        int num_frequency_bins,
        int num_mel_filters);

    // Auto-dispatching optimized versions
    std::vector<float> mel_filter_bank_optimized(
        int num_frequency_bins,
        int num_mel_filters,
        float min_frequency,
        float max_frequency,
        int sampling_rate,
        bool apply_slaney_norm = false);

    void mel_spectrogram_optimized(
        const float* magnitude_spec,
        const float* mel_filters,
        float* out,
        int num_frames,
        int num_frequency_bins,
        int num_mel_filters);

    // ---------------------------------------------------------------
    //  Window function generation
    //  Equivalent to HuggingFace audio_utils.window_function().
    //  Supported window names: "boxcar", "hamming", "hann", "povey"
    // ---------------------------------------------------------------

    // Generate a window of length window_length.
    //
    // window_length:  number of samples in the window
    // name:           "boxcar", "hamming", "hann", or "povey"
    // periodic:       if true, generate periodic window (length+1 then drop last)
    // frame_length:   if > 0, zero-pad or embed window into this size
    // center:         if true and frame_length > 0, center window in the frame
    std::vector<float> window_function(
        int window_length,
        const std::string& name = "hann",
        bool periodic = true,
        int frame_length = 0,
        bool center = true);

    // Auto-dispatching optimized version
    std::vector<float> window_function_optimized(
        int window_length,
        const std::string& name = "hann",
        bool periodic = true,
        int frame_length = 0,
        bool center = true);

    // ---------------------------------------------------------------
    //  Fused unfold + drop-last + window multiply
    //  Extracts overlapping frames from a padded waveform, drops the
    //  last sample of each frame (preemphasis=0 path), and multiplies
    //  element-wise by the window.
    //
    //  waveform:          padded waveform [padded_length]
    //  window:            window coefficients [frame_length]
    //  out_windowed:      output [num_frames * frame_length], row-major
    //  num_frames:        number of output frames
    //  frame_length:      samples per output frame
    //  hop_length:        stride between successive frames
    // ---------------------------------------------------------------
    void apply_window_frames(
        const float* waveform,
        const float* window,
        float* out_windowed,
        int num_frames,
        int frame_length,
        int hop_length);

    void apply_window_frames_optimized(
        const float* waveform,
        const float* window,
        float* out_windowed,
        int num_frames,
        int frame_length,
        int hop_length);

    // ---------------------------------------------------------------
    //  Vectorized log(x + floor)
    //  Computes out[i] = log(in[i] + floor) for i in [0, count).
    // ---------------------------------------------------------------
    void log_mel_floor(
        const float* in,
        float* out,
        int count,
        float floor);

    void log_mel_floor_optimized(
        const float* in,
        float* out,
        int count,
        float floor);

} // namespace audioproc
