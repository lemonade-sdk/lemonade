#include "audio_process_utils/audioproc.hpp"
#include "audio_process_utils/audioprocAVX512.hpp"
#include <algorithm>
// Ensure M_PI is available on MSVC and other platforms that omit it
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace audioproc {

    void rfft_batch(
        const float* frames,
        float* out_complex,
        int num_frames,
        int frame_length,
        int fft_length)
    {
        const int n_bins = fft_length / 2 + 1;

        // Allocate FFTW buffers
        float* fft_in = (float*)fftwf_malloc(fft_length * sizeof(float));
        fftwf_complex* fft_out = (fftwf_complex*)fftwf_malloc(n_bins * sizeof(fftwf_complex));

        // Create plan (FFTW_ESTIMATE for fast planning)
        fftwf_plan plan = fftwf_plan_dft_r2c_1d(fft_length, fft_in, fft_out, FFTW_ESTIMATE);

        for (int f = 0; f < num_frames; ++f) {
            // Copy frame data
            std::memcpy(fft_in, frames + f * frame_length, frame_length * sizeof(float));
            // Zero-pad if fft_length > frame_length
            if (fft_length > frame_length) {
                std::memset(fft_in + frame_length, 0, (fft_length - frame_length) * sizeof(float));
            }

            fftwf_execute(plan);

            // Copy interleaved (re, im) output
            float* dst = out_complex + f * n_bins * 2;
            for (int k = 0; k < n_bins; ++k) {
                dst[2 * k]     = fft_out[k][0]; // real
                dst[2 * k + 1] = fft_out[k][1]; // imag
            }
        }

        fftwf_destroy_plan(plan);
        fftwf_free(fft_in);
        fftwf_free(fft_out);
    }

    void rfft_magnitude_batch(
        const float* frames,
        float* out_magnitude,
        int num_frames,
        int frame_length,
        int fft_length)
    {
        const int n_bins = fft_length / 2 + 1;

        float* fft_in = (float*)fftwf_malloc(fft_length * sizeof(float));
        fftwf_complex* fft_out = (fftwf_complex*)fftwf_malloc(n_bins * sizeof(fftwf_complex));

        fftwf_plan plan = fftwf_plan_dft_r2c_1d(fft_length, fft_in, fft_out, FFTW_ESTIMATE);

        for (int f = 0; f < num_frames; ++f) {
            std::memcpy(fft_in, frames + f * frame_length, frame_length * sizeof(float));
            if (fft_length > frame_length) {
                std::memset(fft_in + frame_length, 0, (fft_length - frame_length) * sizeof(float));
            }

            fftwf_execute(plan);

            float* dst = out_magnitude + f * n_bins;
            for (int k = 0; k < n_bins; ++k) {
                float re = fft_out[k][0];
                float im = fft_out[k][1];
                dst[k] = std::sqrt(re * re + im * im);
            }
        }

        fftwf_destroy_plan(plan);
        fftwf_free(fft_in);
        fftwf_free(fft_out);
    }

    // Auto-dispatching versions
    void rfft_batch_optimized(
        const float* frames,
        float* out_complex,
        int num_frames,
        int frame_length,
        int fft_length)
    {
        if (avx512::has_avx512f()) {
            avx512::rfft_batch_avx512(frames, out_complex, num_frames, frame_length, fft_length);
        } else {
            rfft_batch(frames, out_complex, num_frames, frame_length, fft_length);
        }
    }

    void rfft_magnitude_batch_optimized(
        const float* frames,
        float* out_magnitude,
        int num_frames,
        int frame_length,
        int fft_length)
    {
        if (avx512::has_avx512f()) {
            avx512::rfft_magnitude_batch_avx512(frames, out_magnitude, num_frames, frame_length, fft_length);
        } else {
            rfft_magnitude_batch(frames, out_magnitude, num_frames, frame_length, fft_length);
        }
    }

    // ---------------------------------------------------------------
    //  Mel filter bank — scalar implementation
    // ---------------------------------------------------------------

    std::vector<float> mel_filter_bank(
        int num_frequency_bins,
        int num_mel_filters,
        float min_frequency,
        float max_frequency,
        int sampling_rate,
        bool apply_slaney_norm)
    {
        // mel_freqs: num_mel_filters + 2 linearly spaced points in mel domain
        float mel_min = hertz_to_mel(min_frequency);
        float mel_max = hertz_to_mel(max_frequency);

        const int n_points = num_mel_filters + 2;
        std::vector<float> filter_freqs(n_points);
        for (int i = 0; i < n_points; ++i) {
            float mel = mel_min + (mel_max - mel_min) * i / (n_points - 1);
            filter_freqs[i] = mel_to_hertz(mel);
        }

        // fft_freqs: linearly spaced 0 .. sampling_rate/2
        std::vector<float> fft_freqs(num_frequency_bins);
        float half_sr = static_cast<float>(sampling_rate) / 2.0f;
        for (int i = 0; i < num_frequency_bins; ++i) {
            fft_freqs[i] = half_sr * i / (num_frequency_bins - 1);
        }

        // _create_triangular_filter_bank:
        //   filter_diff = np.diff(filter_freqs)                  [n_points-1]
        //   slopes = fft_freqs[:,None] - filter_freqs[None,:]    sign flipped below
        //   down_slopes = -slopes[:, :-2] / filter_diff[:-1]
        //   up_slopes   =  slopes[:, 2:]  / filter_diff[1:]
        //   result = max(0, min(down_slopes, up_slopes))

        std::vector<float> filter_diff(n_points - 1);
        for (int i = 0; i < n_points - 1; ++i) {
            filter_diff[i] = filter_freqs[i + 1] - filter_freqs[i];
        }

        // Output: [num_frequency_bins x num_mel_filters], row-major
        std::vector<float> mel_filters(
            static_cast<size_t>(num_frequency_bins) * num_mel_filters, 0.0f);

        for (int b = 0; b < num_frequency_bins; ++b) {
            float f = fft_freqs[b];
            for (int m = 0; m < num_mel_filters; ++m) {
                // slope values
                float slope_left  = filter_freqs[m]     - f;   // slopes[:, m]
                float slope_right = filter_freqs[m + 2]  - f;  // slopes[:, m+2]

                float down = -slope_left  / filter_diff[m];     // down_slopes
                float up   =  slope_right / filter_diff[m + 1]; // up_slopes

                float val = std::max(0.0f, std::min(down, up));
                mel_filters[b * num_mel_filters + m] = val;
            }
        }

        // Optional Slaney area-normalization
        if (apply_slaney_norm) {
            for (int m = 0; m < num_mel_filters; ++m) {
                float enorm = 2.0f / (filter_freqs[m + 2] - filter_freqs[m]);
                for (int b = 0; b < num_frequency_bins; ++b) {
                    mel_filters[b * num_mel_filters + m] *= enorm;
                }
            }
        }

        return mel_filters;
    }

    // ---------------------------------------------------------------
    //  Mel spectrogram matmul — scalar implementation
    // ---------------------------------------------------------------

    void mel_spectrogram(
        const float* magnitude_spec,
        const float* mel_filters,
        float* out,
        int num_frames,
        int num_frequency_bins,
        int num_mel_filters)
    {
        // out[f, m] = sum_b( magnitude_spec[f, b] * mel_filters[b, m] )
        for (int f = 0; f < num_frames; ++f) {
            const float* spec_row = magnitude_spec + f * num_frequency_bins;
            float* out_row = out + f * num_mel_filters;

            for (int m = 0; m < num_mel_filters; ++m) {
                float sum = 0.0f;
                for (int b = 0; b < num_frequency_bins; ++b) {
                    sum += spec_row[b] * mel_filters[b * num_mel_filters + m];
                }
                out_row[m] = sum;
            }
        }
    }

    // ---------------------------------------------------------------
    //  Auto-dispatching optimized versions
    // ---------------------------------------------------------------

    std::vector<float> mel_filter_bank_optimized(
        int num_frequency_bins,
        int num_mel_filters,
        float min_frequency,
        float max_frequency,
        int sampling_rate,
        bool apply_slaney_norm)
    {
        if (avx512::has_avx512f()) {
            return avx512::mel_filter_bank_avx512(num_frequency_bins, num_mel_filters,
                                                   min_frequency, max_frequency,
                                                   sampling_rate, apply_slaney_norm);
        } else {
            return mel_filter_bank(num_frequency_bins, num_mel_filters,
                                   min_frequency, max_frequency,
                                   sampling_rate, apply_slaney_norm);
        }
    }

    void mel_spectrogram_optimized(
        const float* magnitude_spec,
        const float* mel_filters,
        float* out,
        int num_frames,
        int num_frequency_bins,
        int num_mel_filters)
    {
        if (avx512::has_avx512f()) {
            avx512::mel_spectrogram_avx512(magnitude_spec, mel_filters, out,
                                            num_frames, num_frequency_bins, num_mel_filters);
        } else {
            mel_spectrogram(magnitude_spec, mel_filters, out,
                            num_frames, num_frequency_bins, num_mel_filters);
        }
    }

    // ---------------------------------------------------------------
    //  Window function — scalar implementation
    // ---------------------------------------------------------------

    std::vector<float> window_function(
        int window_length,
        const std::string& name,
        bool periodic,
        int frame_length,
        bool center)
    {
        const int length = periodic ? window_length + 1 : window_length;
        std::vector<float> window(length);

        if (name == "boxcar") {
            for (int i = 0; i < length; ++i) {
                window[i] = 1.0f;
            }
        } else if (name == "hamming" || name == "hamming_window") {
            // Hamming: 0.54 - 0.46 * cos(2*pi*n / (N-1))
            const float inv = (length > 1) ? 1.0f / (length - 1) : 0.0f;
            for (int i = 0; i < length; ++i) {
                window[i] = 0.54f - 0.46f * std::cos(2.0f * static_cast<float>(M_PI) * i * inv);
            }
        } else if (name == "hann" || name == "hann_window") {
            // Hann: 0.5 - 0.5 * cos(2*pi*n / (N-1))
            const float inv = (length > 1) ? 1.0f / (length - 1) : 0.0f;
            for (int i = 0; i < length; ++i) {
                window[i] = 0.5f - 0.5f * std::cos(2.0f * static_cast<float>(M_PI) * i * inv);
            }
        } else if (name == "povey") {
            // Povey: hann^0.85
            const float inv = (length > 1) ? 1.0f / (length - 1) : 0.0f;
            for (int i = 0; i < length; ++i) {
                float hann = 0.5f - 0.5f * std::cos(2.0f * static_cast<float>(M_PI) * i * inv);
                window[i] = std::pow(hann, 0.85f);
            }
        } else {
            throw std::runtime_error("Unknown window function '" + name + "'");
        }

        // Drop last sample for periodic window
        if (periodic) {
            window.resize(window_length);
        }

        // If no frame_length requested, return the window as-is
        if (frame_length <= 0) {
            return window;
        }

        if (window_length > frame_length) {
            throw std::runtime_error("window_length (" + std::to_string(window_length)
                + ") may not be larger than frame_length (" + std::to_string(frame_length) + ")");
        }

        // Zero-pad / embed into frame_length buffer
        std::vector<float> padded(frame_length, 0.0f);
        int offset = center ? (frame_length - window_length) / 2 : 0;
        std::memcpy(padded.data() + offset, window.data(), window_length * sizeof(float));
        return padded;
    }

    // ---------------------------------------------------------------
    //  Auto-dispatching optimized window_function
    // ---------------------------------------------------------------

    std::vector<float> window_function_optimized(
        int window_length,
        const std::string& name,
        bool periodic,
        int frame_length,
        bool center)
    {
        if (avx512::has_avx512f()) {
            return avx512::window_function_avx512(window_length, name, periodic, frame_length, center);
        } else {
            return window_function(window_length, name, periodic, frame_length, center);
        }
    }

    // ---------------------------------------------------------------
    //  Scalar apply_window_frames
    // ---------------------------------------------------------------

    void apply_window_frames(
        const float* waveform,
        const float* window,
        float* out_windowed,
        int num_frames,
        int frame_length,
        int hop_length)
    {
        for (int f = 0; f < num_frames; f++) {
            const int offset = f * hop_length;
            for (int n = 0; n < frame_length; n++) {
                out_windowed[f * frame_length + n] = waveform[offset + n] * window[n];
            }
        }
    }

    void apply_window_frames_optimized(
        const float* waveform,
        const float* window,
        float* out_windowed,
        int num_frames,
        int frame_length,
        int hop_length)
    {
        if (avx512::has_avx512f()) {
            avx512::apply_window_frames_avx512(waveform, window, out_windowed, num_frames, frame_length, hop_length);
        } else {
            apply_window_frames(waveform, window, out_windowed, num_frames, frame_length, hop_length);
        }
    }

    // ---------------------------------------------------------------
    //  Scalar log_mel_floor
    // ---------------------------------------------------------------

    void log_mel_floor(
        const float* in,
        float* out,
        int count,
        float floor)
    {
        for (int i = 0; i < count; i++) {
            out[i] = std::log(in[i] + floor);
        }
    }

    void log_mel_floor_optimized(
        const float* in,
        float* out,
        int count,
        float floor)
    {
        if (avx512::has_avx512f()) {
            avx512::log_mel_floor_avx512(in, out, count, floor);
        } else {
            log_mel_floor(in, out, count, floor);
        }
    }

} // namespace audioproc
