#pragma once

#include <vector>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <string>
#include <stdexcept>
#include <immintrin.h>
#include "fftw3.h"

#ifdef _MSC_VER
#include <intrin.h>
#endif

namespace audioproc {
namespace avx512 {

    // Check if AVX-512F is available at runtime
    inline bool has_avx512f() {
#ifdef _MSC_VER
        int cpuInfo[4];
        __cpuidex(cpuInfo, 7, 0);
        return (cpuInfo[1] & (1 << 16)) != 0;
#else
        return __builtin_cpu_supports("avx512f");
#endif
    }

    // AVX512 vectorized complex magnitude: sqrt(re^2 + im^2)
    // complex_interleaved: input [n_bins * 2] as (re0, im0, re1, im1, ...)
    // magnitude:           output [n_bins]
    inline void complex_magnitude_avx512(const float* complex_interleaved, float* magnitude, int n_bins) {
        // Permutation indices for deinterleaving real/imag from two concatenated
        // 512-bit vectors (32 floats total → 16 real + 16 imag)
        const __m512i idx_re = _mm512_set_epi32(30, 28, 26, 24, 22, 20, 18, 16,
                                                 14, 12, 10,  8,  6,  4,  2,  0);
        const __m512i idx_im = _mm512_set_epi32(31, 29, 27, 25, 23, 21, 19, 17,
                                                 15, 13, 11,  9,  7,  5,  3,  1);

        int k = 0;
        for (; k + 16 <= n_bins; k += 16) {
            // Load 32 floats (16 complex pairs in interleaved format)
            __m512 v0 = _mm512_loadu_ps(complex_interleaved + 2 * k);
            __m512 v1 = _mm512_loadu_ps(complex_interleaved + 2 * k + 16);

            // Deinterleave real and imaginary parts
            __m512 re = _mm512_permutex2var_ps(v0, idx_re, v1);
            __m512 im = _mm512_permutex2var_ps(v0, idx_im, v1);

            // mag = sqrt(re*re + im*im) using FMA
            __m512 mag_sq = _mm512_fmadd_ps(re, re, _mm512_mul_ps(im, im));
            __m512 mag = _mm512_sqrt_ps(mag_sq);

            _mm512_storeu_ps(magnitude + k, mag);
        }

        // Scalar remainder
        for (; k < n_bins; ++k) {
            float re = complex_interleaved[2 * k];
            float im = complex_interleaved[2 * k + 1];
            magnitude[k] = std::sqrt(re * re + im * im);
        }
    }

    // AVX512 fast zero-fill
    inline void zero_fill_avx512(float* dst, int count) {
        const __m512 zero = _mm512_setzero_ps();
        int i = 0;
        for (; i + 16 <= count; i += 16) {
            _mm512_storeu_ps(dst + i, zero);
        }
        for (; i < count; ++i) {
            dst[i] = 0.0f;
        }
    }

    // AVX512 fast float copy
    inline void copy_float_avx512(const float* src, float* dst, int count) {
        int i = 0;
        for (; i + 16 <= count; i += 16) {
            __m512 v = _mm512_loadu_ps(src + i);
            _mm512_storeu_ps(dst + i, v);
        }
        for (; i < count; ++i) {
            dst[i] = src[i];
        }
    }

    // AVX512 optimized batch rfft using FFTW plan_many
    void rfft_batch_avx512(
        const float* frames,
        float* out_complex,
        int num_frames,
        int frame_length,
        int fft_length);

    // AVX512 optimized fused rfft + magnitude
    void rfft_magnitude_batch_avx512(
        const float* frames,
        float* out_magnitude,
        int num_frames,
        int frame_length,
        int fft_length);

    // AVX512 optimized mel filter bank generation
    std::vector<float> mel_filter_bank_avx512(
        int num_frequency_bins,
        int num_mel_filters,
        float min_frequency,
        float max_frequency,
        int sampling_rate,
        bool apply_slaney_norm = false);

    // AVX512 optimized matmul: out[F,M] = magnitude_spec[F,B] @ mel_filters[B,M]
    // Uses vectorized dot-product along the frequency-bin dimension.
    void mel_spectrogram_avx512(
        const float* magnitude_spec,
        const float* mel_filters,
        float* out,
        int num_frames,
        int num_frequency_bins,
        int num_mel_filters);

    // AVX512 optimized window function generation
    std::vector<float> window_function_avx512(
        int window_length,
        const std::string& name = "hann",
        bool periodic = true,
        int frame_length = 0,
        bool center = true);

    // AVX512 fused unfold + drop-last + window multiply
    void apply_window_frames_avx512(
        const float* waveform,
        const float* window,
        float* out_windowed,
        int num_frames,
        int frame_length,
        int hop_length);

    // AVX512 vectorized log(x + floor)
    void log_mel_floor_avx512(
        const float* in,
        float* out,
        int count,
        float floor);

} // namespace avx512
} // namespace audioproc
