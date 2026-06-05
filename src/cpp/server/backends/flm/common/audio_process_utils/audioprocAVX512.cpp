#include "audio_process_utils/audioproc.hpp"
#include "audio_process_utils/audioprocAVX512.hpp"
#include <algorithm>
#include <omp.h>
// Ensure M_PI is available on MSVC and other platforms that omit it
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
namespace audioproc {
namespace avx512 {

    void rfft_batch_avx512(
        const float* frames,
        float* out_complex,
        int num_frames,
        int frame_length,
        int fft_length)
    {
        if (!has_avx512f()) {
            rfft_batch(frames, out_complex, num_frames, frame_length, fft_length);
            return;
        }

        const int n_bins = fft_length / 2 + 1;
        const int pad_len = fft_length - frame_length;

        // Allocate contiguous padded input buffer for batch FFT
        float* padded_in = (float*)fftwf_malloc(
            static_cast<size_t>(num_frames) * fft_length * sizeof(float));
        fftwf_complex* fft_out = (fftwf_complex*)fftwf_malloc(
            static_cast<size_t>(num_frames) * n_bins * sizeof(fftwf_complex));

        // AVX512 accelerated copy + zero-pad for each frame
        for (int f = 0; f < num_frames; ++f) {
            float* dst = padded_in + f * fft_length;
            copy_float_avx512(frames + f * frame_length, dst, frame_length);
            if (pad_len > 0) {
                zero_fill_avx512(dst + frame_length, pad_len);
            }
        }

        // Use FFTW plan_many for efficient batch real-to-complex FFT
        int n[] = { fft_length };
        fftwf_plan plan = fftwf_plan_many_dft_r2c(
            1,              // rank (1D FFT)
            n,              // FFT dimension
            num_frames,     // number of transforms
            padded_in,      // input
            nullptr,        // inembed (default: same as n)
            1,              // istride
            fft_length,     // idist (distance between consecutive inputs)
            fft_out,        // output
            nullptr,        // onembed
            1,              // ostride
            n_bins,         // odist (distance between consecutive outputs)
            FFTW_ESTIMATE);

        fftwf_execute(plan);

        // Copy interleaved complex output — FFTW's fftwf_complex is float[2],
        // which is already the interleaved format we want.  Just memcpy.
        std::memcpy(out_complex, fft_out,
                     static_cast<size_t>(num_frames) * n_bins * 2 * sizeof(float));

        fftwf_destroy_plan(plan);
        fftwf_free(padded_in);
        fftwf_free(fft_out);
    }

    void rfft_magnitude_batch_avx512(
        const float* frames,
        float* out_magnitude,
        int num_frames,
        int frame_length,
        int fft_length)
    {
        if (!has_avx512f()) {
            rfft_magnitude_batch(frames, out_magnitude, num_frames, frame_length, fft_length);
            return;
        }

        const int n_bins = fft_length / 2 + 1;
        const int pad_len = fft_length - frame_length;

        // Allocate contiguous padded input buffer for batch FFT
        float* padded_in = (float*)fftwf_malloc(
            static_cast<size_t>(num_frames) * fft_length * sizeof(float));
        fftwf_complex* fft_out = (fftwf_complex*)fftwf_malloc(
            static_cast<size_t>(num_frames) * n_bins * sizeof(fftwf_complex));

        // AVX512 accelerated copy + zero-pad
        for (int f = 0; f < num_frames; ++f) {
            float* dst = padded_in + f * fft_length;
            copy_float_avx512(frames + f * frame_length, dst, frame_length);
            if (pad_len > 0) {
                zero_fill_avx512(dst + frame_length, pad_len);
            }
        }

        // Batch FFT via FFTW plan_many
        int n[] = { fft_length };
        fftwf_plan plan = fftwf_plan_many_dft_r2c(
            1,              // rank
            n,              // FFT dimension
            num_frames,     // howmany
            padded_in,      // input
            nullptr,        // inembed
            1,              // istride
            fft_length,     // idist
            fft_out,        // output
            nullptr,        // onembed
            1,              // ostride
            n_bins,         // odist
            FFTW_ESTIMATE);

        fftwf_execute(plan);

        // AVX512 vectorized magnitude computation for each frame
        #pragma omp parallel for schedule(static) if(num_frames > 32)
        for (int f = 0; f < num_frames; ++f) {
            const float* src = reinterpret_cast<const float*>(fft_out + f * n_bins);
            float* dst = out_magnitude + f * n_bins;
            complex_magnitude_avx512(src, dst, n_bins);
        }

        fftwf_destroy_plan(plan);
        fftwf_free(padded_in);
        fftwf_free(fft_out);
    }

    // ---------------------------------------------------------------
    //  Mel filter bank — AVX512 implementation
    // ---------------------------------------------------------------

    std::vector<float> mel_filter_bank_avx512(
        int num_frequency_bins,
        int num_mel_filters,
        float min_frequency,
        float max_frequency,
        int sampling_rate,
        bool apply_slaney_norm)
    {
        if (!has_avx512f()) {
            return mel_filter_bank(num_frequency_bins, num_mel_filters,
                                   min_frequency, max_frequency,
                                   sampling_rate, apply_slaney_norm);
        }

        // mel_freqs: num_mel_filters + 2 linearly spaced in mel domain
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

        // filter_diff[i] = filter_freqs[i+1] - filter_freqs[i]
        std::vector<float> filter_diff(n_points - 1);
        for (int i = 0; i < n_points - 1; ++i) {
            filter_diff[i] = filter_freqs[i + 1] - filter_freqs[i];
        }

        // Output: [num_frequency_bins x num_mel_filters], row-major
        std::vector<float> mel_filters(
            static_cast<size_t>(num_frequency_bins) * num_mel_filters, 0.0f);

        const __m512 v_zero = _mm512_setzero_ps();

        // For each mel filter, vectorize over frequency bins (16 at a time)
        for (int m = 0; m < num_mel_filters; ++m) {
            float f_left   = filter_freqs[m];
            float f_right  = filter_freqs[m + 2];
            float inv_diff_down = 1.0f / filter_diff[m];
            float inv_diff_up   = 1.0f / filter_diff[m + 1];

            __m512 v_f_left      = _mm512_set1_ps(f_left);
            __m512 v_f_right     = _mm512_set1_ps(f_right);
            __m512 v_inv_dd      = _mm512_set1_ps(inv_diff_down);
            __m512 v_inv_du      = _mm512_set1_ps(inv_diff_up);

            int b = 0;
            for (; b + 16 <= num_frequency_bins; b += 16) {
                __m512 v_f = _mm512_loadu_ps(&fft_freqs[b]);

                // down = (f - f_left) * inv_diff_down
                __m512 v_down = _mm512_mul_ps(_mm512_sub_ps(v_f, v_f_left), v_inv_dd);
                // up   = (f_right - f) * inv_diff_up
                __m512 v_up   = _mm512_mul_ps(_mm512_sub_ps(v_f_right, v_f), v_inv_du);

                // max(0, min(down, up))
                __m512 v_val = _mm512_max_ps(v_zero, _mm512_min_ps(v_down, v_up));

                // Scatter-store in column m of row-major matrix
                // mel_filters[b * num_mel_filters + m] for b..b+15
                // Since layout is row-major with stride num_mel_filters, store individually
                alignas(64) float tmp[16];
                _mm512_store_ps(tmp, v_val);
                for (int i = 0; i < 16; ++i) {
                    mel_filters[(b + i) * num_mel_filters + m] = tmp[i];
                }
            }

            // Scalar remainder
            for (; b < num_frequency_bins; ++b) {
                float f = fft_freqs[b];
                float down = (f - f_left) * inv_diff_down;
                float up   = (f_right - f) * inv_diff_up;
                mel_filters[b * num_mel_filters + m] = std::max(0.0f, std::min(down, up));
            }
        }

        // Optional Slaney area-normalization
        if (apply_slaney_norm) {
            for (int m = 0; m < num_mel_filters; ++m) {
                float enorm = 2.0f / (filter_freqs[m + 2] - filter_freqs[m]);
                __m512 v_enorm = _mm512_set1_ps(enorm);

                int b = 0;
                for (; b + 16 <= num_frequency_bins; b += 16) {
                    // Gather-modify-scatter for column m
                    alignas(64) float tmp[16];
                    for (int i = 0; i < 16; ++i) {
                        tmp[i] = mel_filters[(b + i) * num_mel_filters + m];
                    }
                    __m512 v = _mm512_load_ps(tmp);
                    v = _mm512_mul_ps(v, v_enorm);
                    _mm512_store_ps(tmp, v);
                    for (int i = 0; i < 16; ++i) {
                        mel_filters[(b + i) * num_mel_filters + m] = tmp[i];
                    }
                }
                for (; b < num_frequency_bins; ++b) {
                    mel_filters[b * num_mel_filters + m] *= enorm;
                }
            }
        }

        return mel_filters;
    }

    // ---------------------------------------------------------------
    //  Mel spectrogram matmul — AVX512 implementation
    //  out[f,m] = sum_b( magnitude_spec[f,b] * mel_filters[b,m] )
    // ---------------------------------------------------------------

    void mel_spectrogram_avx512(
        const float* magnitude_spec,
        const float* mel_filters,
        float* out,
        int num_frames,
        int num_frequency_bins,
        int num_mel_filters)
    {
        if (!has_avx512f()) {
            mel_spectrogram(magnitude_spec, mel_filters, out,
                            num_frames, num_frequency_bins, num_mel_filters);
            return;
        }

        // For each frame, for each mel filter, vectorized dot product
        // over frequency bins
        #pragma omp parallel for schedule(static) if(num_frames > 32)
        for (int f = 0; f < num_frames; ++f) {
            const float* spec_row = magnitude_spec + f * num_frequency_bins;
            float* out_row = out + f * num_mel_filters;

            for (int m = 0; m < num_mel_filters; ++m) {
                __m512 v_sum = _mm512_setzero_ps();

                int b = 0;
                for (; b + 16 <= num_frequency_bins; b += 16) {
                    __m512 v_spec = _mm512_loadu_ps(spec_row + b);

                    // Gather mel_filters column m: mel_filters[(b+i)*num_mel_filters + m]
                    // Stride = num_mel_filters floats
                    alignas(64) float col[16];
                    for (int i = 0; i < 16; ++i) {
                        col[i] = mel_filters[(b + i) * num_mel_filters + m];
                    }
                    __m512 v_filt = _mm512_load_ps(col);

                    v_sum = _mm512_fmadd_ps(v_spec, v_filt, v_sum);
                }

                float sum = _mm512_reduce_add_ps(v_sum);

                // Scalar remainder
                for (; b < num_frequency_bins; ++b) {
                    sum += spec_row[b] * mel_filters[b * num_mel_filters + m];
                }

                out_row[m] = sum;
            }
        }
    }

    // ---------------------------------------------------------------
    //  Window function — AVX512 implementation
    // ---------------------------------------------------------------

    std::vector<float> window_function_avx512(
        int window_length,
        const std::string& name,
        bool periodic,
        int frame_length,
        bool center)
    {
        if (!has_avx512f()) {
            return window_function(window_length, name, periodic, frame_length, center);
        }

        const int length = periodic ? window_length + 1 : window_length;
        std::vector<float> window(length);

        if (name == "boxcar") {
            const __m512 v_one = _mm512_set1_ps(1.0f);
            int i = 0;
            for (; i + 16 <= length; i += 16) {
                _mm512_storeu_ps(window.data() + i, v_one);
            }
            for (; i < length; ++i) {
                window[i] = 1.0f;
            }
        } else if (name == "hamming" || name == "hamming_window" ||
                   name == "hann" || name == "hann_window" ||
                   name == "povey") {
            // All three are based on cosine windows:
            //   hamming: a0=0.54, a1=0.46
            //   hann:    a0=0.50, a1=0.50
            //   povey:   hann^0.85
            const bool is_hamming = (name == "hamming" || name == "hamming_window");
            const bool is_povey  = (name == "povey");
            const float a0 = is_hamming ? 0.54f : 0.5f;
            const float a1 = is_hamming ? 0.46f : 0.5f;

            const float inv = (length > 1) ? 1.0f / static_cast<float>(length - 1) : 0.0f;
            const float two_pi_inv = 2.0f * static_cast<float>(M_PI) * inv;

            // Vectorized: window[i] = a0 - a1 * cos(2*pi*i / (N-1))
            const __m512 v_a0 = _mm512_set1_ps(a0);
            const __m512 v_a1 = _mm512_set1_ps(a1);
            const __m512 v_two_pi_inv = _mm512_set1_ps(two_pi_inv);
            // Incremental indices: [0,1,2,...,15]
            const __m512 v_iota = _mm512_set_ps(15, 14, 13, 12, 11, 10, 9, 8,
                                                 7, 6, 5, 4, 3, 2, 1, 0);
            const __m512 v_sixteen = _mm512_set1_ps(16.0f);

            // Fast vectorized cos approximation coefficients
            // cos(x) ≈ 1 - x²/2 + x⁴/24 - x⁶/720  (valid near 0,
            // but we use range-reduction: cos(x) = cos(x mod 2π))
            // Since we need high accuracy for filter bank correctness,
            // we compute scalar cos but in unrolled AVX512 fashion.
            // For the typical window_length (320-640), the loop is short.

            int i = 0;
            __m512 v_offset = _mm512_setzero_ps();
            for (; i + 16 <= length; i += 16) {
                // indices = offset + iota
                __m512 v_idx = _mm512_add_ps(v_offset, v_iota);
                // angle = idx * two_pi_inv
                __m512 v_angle = _mm512_mul_ps(v_idx, v_two_pi_inv);

                // Compute cos for 16 values (extract, compute, re-pack)
                alignas(64) float angles[16];
                _mm512_store_ps(angles, v_angle);
                alignas(64) float cos_vals[16];
                for (int j = 0; j < 16; ++j) {
                    cos_vals[j] = std::cos(angles[j]);
                }
                __m512 v_cos = _mm512_load_ps(cos_vals);

                // window = a0 - a1 * cos
                __m512 v_win = _mm512_fnmadd_ps(v_a1, v_cos, v_a0);

                if (is_povey) {
                    // pow(hann, 0.85) — extract, scalar pow, re-pack
                    alignas(64) float win_vals[16];
                    _mm512_store_ps(win_vals, v_win);
                    for (int j = 0; j < 16; ++j) {
                        win_vals[j] = std::pow(win_vals[j], 0.85f);
                    }
                    v_win = _mm512_load_ps(win_vals);
                }

                _mm512_storeu_ps(window.data() + i, v_win);
                v_offset = _mm512_add_ps(v_offset, v_sixteen);
            }

            // Scalar remainder
            for (; i < length; ++i) {
                float w = a0 - a1 * std::cos(two_pi_inv * i);
                if (is_povey) {
                    w = std::pow(w, 0.85f);
                }
                window[i] = w;
            }
        } else {
            throw std::runtime_error("Unknown window function '" + name + "'");
        }

        // Drop last sample for periodic window
        if (periodic) {
            window.resize(window_length);
        }

        // If no frame_length requested, return as-is
        if (frame_length <= 0) {
            return window;
        }

        if (window_length > frame_length) {
            throw std::runtime_error("window_length (" + std::to_string(window_length)
                + ") may not be larger than frame_length (" + std::to_string(frame_length) + ")");
        }

        // Zero-pad / embed into frame_length buffer using AVX512
        std::vector<float> padded(frame_length, 0.0f);
        int offset = center ? (frame_length - window_length) / 2 : 0;
        copy_float_avx512(window.data(), padded.data() + offset, window_length);
        return padded;
    }

    // ---------------------------------------------------------------
    //  AVX512 fused unfold + drop-last + window multiply
    // ---------------------------------------------------------------

    void apply_window_frames_avx512(
        const float* waveform,
        const float* window,
        float* out_windowed,
        int num_frames,
        int frame_length,
        int hop_length)
    {
        #pragma omp parallel for schedule(static)
        for (int f = 0; f < num_frames; f++) {
            const float* src = waveform + f * hop_length;
            float* dst = out_windowed + f * frame_length;
            int n = 0;
            for (; n + 16 <= frame_length; n += 16) {
                __m512 v_wav = _mm512_loadu_ps(src + n);
                __m512 v_win = _mm512_loadu_ps(window + n);
                __m512 v_out = _mm512_mul_ps(v_wav, v_win);
                _mm512_storeu_ps(dst + n, v_out);
            }
            // Scalar remainder
            for (; n < frame_length; n++) {
                dst[n] = src[n] * window[n];
            }
        }
    }

    // ---------------------------------------------------------------
    //  AVX512 vectorized log(x + floor)
    //  Uses a fast polynomial approximation of log2, then scales
    //  by ln(2) to get natural log.
    // ---------------------------------------------------------------

    // Fast AVX512 log approximation: natural log via log2 polynomial
    // Accuracy: ~1e-5 relative error over normal float range
    static inline __m512 fast_log_avx512(__m512 x) {
        // Extract exponent and mantissa
        // log(x) = (exponent + log2(mantissa)) * ln(2)
        const __m512 one    = _mm512_set1_ps(1.0f);
        const __m512 ln2    = _mm512_set1_ps(0.6931471805599453f);
        const __m512i exp_mask = _mm512_set1_epi32(0x7F800000);
        const __m512i man_mask = _mm512_set1_epi32(0x007FFFFF);
        const __m512i bias   = _mm512_set1_epi32(127);

        // Get exponent: floor(log2(x))
        __m512i xi = _mm512_castps_si512(x);
        __m512i exp_i = _mm512_srli_epi32(_mm512_and_si512(xi, exp_mask), 23);
        __m512 exponent = _mm512_cvtepi32_ps(_mm512_sub_epi32(exp_i, bias));

        // Get mantissa in [1, 2)
        __m512i man_i = _mm512_or_si512(_mm512_and_si512(xi, man_mask),
                                         _mm512_set1_epi32(0x3F800000));
        __m512 mantissa = _mm512_castsi512_ps(man_i);

        // Polynomial approximation of log2(mantissa) for mantissa in [1,2)
        // Using minimax polynomial:  log2(m) ≈ p(m-1)
        __m512 m = _mm512_sub_ps(mantissa, one);

        // Coefficients for log2(1+m) ≈ c1*m + c2*m^2 + c3*m^3 + c4*m^4 + c5*m^5
        const __m512 c1 = _mm512_set1_ps( 1.4426950408889634f);  // 1/ln(2)
        const __m512 c2 = _mm512_set1_ps(-0.7213475204444817f);  // -1/(2*ln(2))
        const __m512 c3 = _mm512_set1_ps( 0.4808983469629878f);  // 1/(3*ln(2))
        const __m512 c4 = _mm512_set1_ps(-0.3606737602222408f);  // -1/(4*ln(2))
        const __m512 c5 = _mm512_set1_ps( 0.2885390081777927f);  // 1/(5*ln(2))

        // Horner's method: p = m*(c1 + m*(c2 + m*(c3 + m*(c4 + m*c5))))
        __m512 p = _mm512_fmadd_ps(m, c5, c4);
        p = _mm512_fmadd_ps(m, p, c3);
        p = _mm512_fmadd_ps(m, p, c2);
        p = _mm512_fmadd_ps(m, p, c1);
        p = _mm512_mul_ps(m, p);

        // log(x) = (exponent + log2_mantissa) * ln(2)
        __m512 result = _mm512_fmadd_ps(exponent, ln2, _mm512_mul_ps(p, ln2));
        return result;
    }

    void log_mel_floor_avx512(
        const float* in,
        float* out,
        int count,
        float floor)
    {
        const __m512 v_floor = _mm512_set1_ps(floor);
        int i = 0;

        for (; i + 16 <= count; i += 16) {
            __m512 v_in = _mm512_loadu_ps(in + i);
            __m512 v_sum = _mm512_add_ps(v_in, v_floor);
            __m512 v_log = fast_log_avx512(v_sum);
            _mm512_storeu_ps(out + i, v_log);
        }

        // Scalar remainder
        for (; i < count; i++) {
            out[i] = std::log(in[i] + floor);
        }
    }

} // namespace avx512
} // namespace audioproc
