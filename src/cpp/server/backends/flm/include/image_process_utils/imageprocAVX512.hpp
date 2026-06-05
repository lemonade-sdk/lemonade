#pragma once

#include <vector>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <immintrin.h>
#include "typedef.hpp"

#ifdef _MSC_VER
#include <intrin.h>
#endif

namespace imgproc {
namespace avx512 {

    // Check if AVX-512F is available at runtime
    inline bool has_avx512f() {
#ifdef _MSC_VER
        // MSVC-specific CPU detection
        int cpuInfo[4];
        __cpuidex(cpuInfo, 7, 0);
        // AVX-512F is bit 16 of EBX
        return (cpuInfo[1] & (1 << 16)) != 0;
#else
        // GCC/Clang-specific CPU detection
        return __builtin_cpu_supports("avx512f");
#endif
    }



    // Fast, corrected AVX-512 exp approximation (single-precision).
    // Notes:
    //  - Input x is clamped to [-88, 88] to avoid overflow/underflow.
    //  - Uses range reduction x = n*ln2 + r, where n is rounded to nearest int.
    //  - Uses a degree-5 polynomial for exp(r) evaluated with Horner + FMAs.
    //  - Constructs 2^n by writing the biased exponent field; the biased exponent
    //    is clamped to [0,255] as a safety measure.
    //
    // This is an approximation (not fully IEEE-754 accurate for all cases).
    inline __m512 _mm512_exp_ps_corrected(__m512 x) {
        // clamp x to a reasonable range to avoid overflow/underflow
        const __m512 max_val = _mm512_set1_ps(88.0f);
        const __m512 min_val = _mm512_set1_ps(-88.0f);
        x = _mm512_min_ps(x, max_val);
        x = _mm512_max_ps(x, min_val);

        // constants: 1/ln2 and split ln2 = ln2_hi + ln2_lo for extra precision
        const __m512 ln2_inv = _mm512_set1_ps(1.44269504088896341f);  // 1/ln(2)
        const __m512 ln2_hi  = _mm512_set1_ps(0.6931471824645996f);   // hi part
        const __m512 ln2_lo  = _mm512_set1_ps(1.9082149292705877e-10f);// lo part

        // compute fx = x * (1/ln2)
        __m512 fx = _mm512_mul_ps(x, ln2_inv);

        // round to nearest integer (using rounding intrinsic), storing integer-valued floats
        fx = _mm512_roundscale_ps(fx, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);

        // convert to int32 (safe since fx holds integer values after rounding)
        __m512i emm0 = _mm512_cvttps_epi32(fx);

        // convert back to float for range-reduction arithmetic
        __m512 n_ps = _mm512_cvtepi32_ps(emm0);

        // r = x - n * ln2  (use fnmadd to compute c - a*b robustly)
        // first r1 = x - n*ln2_hi
        __m512 r = _mm512_fnmadd_ps(n_ps, ln2_hi, x);  // r = x - n*ln2_hi
        // then r = r - n*ln2_lo
        r = _mm512_fnmadd_ps(n_ps, ln2_lo, r);         // r = x - n*(ln2_hi + ln2_lo)

        // polynomial coefficients for exp(r) ~ 1 + r + r^2/2 + r^3/6 + r^4/24 + r^5/120
        const __m512 c5 = _mm512_set1_ps(0.008333333333333333f);  // 1/120
        const __m512 c4 = _mm512_set1_ps(0.041666666666666664f);  // 1/24
        const __m512 c3 = _mm512_set1_ps(0.16666666666666666f);   // 1/6
        const __m512 c2 = _mm512_set1_ps(0.5f);                   // 1/2
        const __m512 c1 = _mm512_set1_ps(1.0f);
        const __m512 one = _mm512_set1_ps(1.0f);

        // Horner evaluation using FMA: (((c5*r + c4)*r + c3)*r + c2)*r + c1 ; then final *r + 1
        __m512 y = _mm512_fmadd_ps(c5, r, c4);
        y = _mm512_fmadd_ps(y, r, c3);
        y = _mm512_fmadd_ps(y, r, c2);
        y = _mm512_fmadd_ps(y, r, c1);
        y = _mm512_fmadd_ps(y, r, one); // y now approximates exp(r)

        // Build 2^n by inserting biased exponent into float bits:
        // biased = n + 127
        __m512i biased = _mm512_add_epi32(emm0, _mm512_set1_epi32(127));

        // clamp biased exponent to [0,255] to avoid invalid bit patterns
        biased = _mm512_max_epi32(biased, _mm512_set1_epi32(0));
        biased = _mm512_min_epi32(biased, _mm512_set1_epi32(255));

        // shift into exponent position (bits 23..30) and reinterpret as float
        biased = _mm512_slli_epi32(biased, 23);
        __m512 pow2n = _mm512_castsi512_ps(biased);

        // final result: exp(x) ≈ exp(r) * 2^n
        return _mm512_mul_ps(y, pow2n);
    }

    // Vectorized gaussian function for 16 floats using AVX-512 exp approximation
    inline __m512 gaussian_avx512(__m512 x, __m512 sigma) {
        const __m512 one = _mm512_set1_ps(1.0f);
        const __m512 two = _mm512_set1_ps(2.0f);
        const __m512 zero = _mm512_setzero_ps();

        // Check if sigma <= 0
        __mmask16 mask_zero_sigma = _mm512_cmp_ps_mask(sigma, zero, _CMP_LE_OQ);

        // Compute exp(-(x*x)/(2*sigma*sigma)) using fast AVX-512 approximation
        __m512 x_sq = _mm512_mul_ps(x, x);
        __m512 sigma_sq = _mm512_mul_ps(sigma, sigma);
        __m512 two_sigma_sq = _mm512_mul_ps(two, sigma_sq);

        // Compute -(x*x)/(2*sigma*sigma)
        __m512 neg_x_sq_over_2sigma_sq = _mm512_div_ps(_mm512_sub_ps(zero, x_sq), two_sigma_sq);

        // Apply fast exponential
        __m512 exp_result = _mm512_exp_ps_corrected(neg_x_sq_over_2sigma_sq);

        // Return 1.0 if sigma <= 0, otherwise exp result
        return _mm512_mask_blend_ps(mask_zero_sigma, exp_result, one);
    }

    // Fast conversion from uint8 to float with normalization
    inline void convert_uint8_to_float_avx512(const uint8_t* src, float* dst, size_t count) {
        const size_t simd_count = count & ~15; // Process in chunks of 16

        for (size_t i = 0; i < simd_count; i += 16) {
            // Load 16 uint8 values
            __m128i u8_vec = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i));

            // Convert to 32-bit integers
            __m512i i32_vec = _mm512_cvtepu8_epi32(u8_vec);

            // Convert to float
            __m512 f32_vec = _mm512_cvtepi32_ps(i32_vec);

            // Store result
            _mm512_storeu_ps(dst + i, f32_vec);
        }

        // Handle remaining elements
        for (size_t i = simd_count; i < count; ++i) {
            dst[i] = static_cast<float>(src[i]);
        }
    }

    // Fast conversion from float to uint8 with clamping
    inline void convert_float_to_uint8_avx512(const float* src, uint8_t* dst, size_t count) {
        const __m512 zero = _mm512_setzero_ps();
        const __m512 max_val = _mm512_set1_ps(255.0f);
        const size_t simd_count = count & ~15; // Process in chunks of 16

        for (size_t i = 0; i < simd_count; i += 16) {
            // Load 16 float values
            __m512 f32_vec = _mm512_loadu_ps(src + i);

            // Round to nearest integer
            f32_vec = _mm512_roundscale_ps(f32_vec, _MM_FROUND_TO_NEAREST_INT);

            // Clamp to [0, 255]
            f32_vec = _mm512_max_ps(f32_vec, zero);
            f32_vec = _mm512_min_ps(f32_vec, max_val);

            // Convert to 32-bit integers
            __m512i i32_vec = _mm512_cvtps_epi32(f32_vec);

            // Pack to uint8 (with saturation)
            __m128i u8_vec = _mm512_cvtusepi32_epi8(i32_vec);

            // Store result
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), u8_vec);
        }

        // Handle remaining elements
        for (size_t i = simd_count; i < count; ++i) {
            dst[i] = static_cast<uint8_t>(std::clamp(std::round(src[i]), 0.0f, 255.0f));
        }
    }



    // Vectorized bicubic kernel computation for 16 floats
    inline __m512 bicubic_kernel_avx512(__m512 x) {
        const __m512 a = _mm512_set1_ps(-0.5f);
        const __m512 one = _mm512_set1_ps(1.0f);
        const __m512 two = _mm512_set1_ps(2.0f);
        const __m512 three = _mm512_set1_ps(3.0f);
        const __m512 four = _mm512_set1_ps(4.0f);
        const __m512 five = _mm512_set1_ps(5.0f);
        const __m512 eight = _mm512_set1_ps(8.0f);
        const __m512 zero = _mm512_setzero_ps();

        // Take absolute value
        x = _mm512_abs_ps(x);

        // Mask for x < 1.0
        __mmask16 mask_lt1 = _mm512_cmp_ps_mask(x, one, _CMP_LT_OQ);
        // Mask for x < 2.0
        __mmask16 mask_lt2 = _mm512_cmp_ps_mask(x, two, _CMP_LT_OQ);
        // Mask for 1.0 <= x < 2.0
        __mmask16 mask_1to2 = _kand_mask16(mask_lt2, _knot_mask16(mask_lt1));

        // Case 1: x < 1.0
        // ((a + 2.0f) * x - (a + 3.0f)) * x * x + 1.0f
        __m512 a_plus_2 = _mm512_add_ps(a, two);
        __m512 a_plus_3 = _mm512_add_ps(a, three);
        __m512 temp1 = _mm512_sub_ps(_mm512_mul_ps(a_plus_2, x), a_plus_3);
        __m512 x_sq = _mm512_mul_ps(x, x);
        __m512 result1 = _mm512_add_ps(_mm512_mul_ps(_mm512_mul_ps(temp1, x), x_sq), one);

        // Case 2: 1.0 <= x < 2.0
        // ((a * x - 5.0f * a) * x + 8.0f * a) * x - 4.0f * a
        __m512 five_a = _mm512_mul_ps(five, a);
        __m512 eight_a = _mm512_mul_ps(eight, a);
        __m512 four_a = _mm512_mul_ps(four, a);
        __m512 temp2 = _mm512_add_ps(_mm512_mul_ps(_mm512_sub_ps(_mm512_mul_ps(a, x), five_a), x), eight_a);
        __m512 result2 = _mm512_sub_ps(_mm512_mul_ps(temp2, x), four_a);

        // Combine results based on masks
        __m512 result = _mm512_mask_blend_ps(mask_lt1, zero, result1);
        result = _mm512_mask_blend_ps(mask_1to2, result, result2);

        return result;
    }


    // AVX-512 optimized bicubic resize for a single plane
    std::vector<float> resize_bicubic_plane_avx512(
        const std::vector<float>& src,
        int src_w, int src_h,
        int dst_w, int dst_h,
        bool antialias);

    // AVX-512 optimized main RGB planar entry point
    std::vector<uint8_t> resize_bicubic_antialias_rgb_planar_avx512(
        const uint8_t* src,
        int src_w, int src_h,
        int dst_w, int dst_h,
        bool antialias);

    // AVX-512 optimized rescale and normalize function
    void rescale_and_normalize_avx512(
        const uint8_t *image_src,
        float *output_buffer,
        int image_width, int image_height, int image_channels,
        bool do_rescale,
        float rescale_factor,
        bool do_normalize,
        float image_mean,
        float image_std
    );

    // AVX-512 optimized rescale and normalize function (per-channel mean/std)
    void rescale_and_normalize_avx512(
        const uint8_t *image_src,
        float *output_buffer,
        int image_width, int image_height, int image_channels,
        bool do_rescale,
        float rescale_factor,
        bool do_normalize,
        const std::vector<float>& image_mean,
        const std::vector<float>& image_std
    );

} // namespace avx512
} // namespace imgproc
