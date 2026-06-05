/// \file metrices.hpp
/// \brief metrices class
/// \author FastFlowLM Team
/// \date 2025-06-24
/// \version 0.9.24
/// \note This class is used to calculate the numerical error metrics.
#pragma once

#include "typedef.hpp"

typedef struct {
    /// \brief cosine similarity
    float CosineSimilarity;
    /// \brief relative L1
    float RelativeL1;
    /// \brief RMSE
    float RMSE;
    /// \brief relative L2
    float RelativeL2;
} error_metrics;

/// \brief AVX2 Absolute value
/// \param x the input value, 256-bits, 8xfloat32
/// \return the absolute value, 256-bits, 8xfloat32
inline __m256 _mm256_abs_ps(__m256 x){
    return _mm256_and_ps(x, _mm256_set1_ps(0x7fffffff));
}

/// \brief get the error metrics
/// \param y the output value, 256-bits, 8xbf16
/// \param y_ref the reference value, 256-bits, 8xbf16
/// \return the error metrics
inline error_metrics get_error_metrics(buffer<bf16>& y, buffer<bf16>& y_ref){
    assert(y.size() == y_ref.size());
    error_metrics metrics;
    const int simd_width = 8;

    __m256 dot_product = _mm256_setzero_ps();
    __m256 y_square_sum = _mm256_setzero_ps();
    __m256 y_ref_square_sum = _mm256_setzero_ps();
    __m256 error_square_sum = _mm256_setzero_ps();
    __m256 abs_error_sum = _mm256_setzero_ps();
    __m256 abs_y_ref_sum = _mm256_setzero_ps();
    for (int i = 0; i < y.size(); i += simd_width){
        __m128i y_bf16  = _mm_loadu_si128((__m128i*)(y.data()  + i));
        __m128i y_ref_bf16 = _mm_loadu_si128((__m128i*)(y_ref.data() + i));
        __m256  y_vec = bf16o_fp32(y_bf16);
        __m256  y_ref_vec = bf16o_fp32(y_ref_bf16);
        __m256 error_vec = _mm256_sub_ps(y_vec, y_ref_vec);
        y_square_sum = _mm256_add_ps(y_square_sum, _mm256_mul_ps(y_vec, y_vec));
        y_ref_square_sum = _mm256_add_ps(y_ref_square_sum, _mm256_mul_ps(y_ref_vec, y_ref_vec));
        dot_product = _mm256_add_ps(dot_product, _mm256_mul_ps(y_vec, y_ref_vec));
        error_square_sum = _mm256_add_ps(error_square_sum, _mm256_mul_ps(error_vec, error_vec));
        abs_error_sum = _mm256_add_ps(abs_error_sum, _mm256_abs_ps(error_vec));
        abs_y_ref_sum = _mm256_add_ps(abs_y_ref_sum, _mm256_abs_ps(y_ref_vec));
    }
    f32 temp[simd_width];
    _mm256_storeu_ps(temp, dot_product);
    f32 dot_product_sum = temp[0] + temp[1] + temp[2] + temp[3] +
                      temp[4] + temp[5] + temp[6] + temp[7];
    _mm256_storeu_ps(temp, y_square_sum);
    f32 y_square_sum_sum = temp[0] + temp[1] + temp[2] + temp[3] +
                      temp[4] + temp[5] + temp[6] + temp[7];
    _mm256_storeu_ps(temp, y_ref_square_sum);
    f32 y_ref_square_sum_sum = temp[0] + temp[1] + temp[2] + temp[3] +
                      temp[4] + temp[5] + temp[6] + temp[7];
    _mm256_storeu_ps(temp, error_square_sum);
    f32 error_square_sum_sum = temp[0] + temp[1] + temp[2] + temp[3] +
                      temp[4] + temp[5] + temp[6] + temp[7];
    _mm256_storeu_ps(temp, abs_error_sum);
    f32 abs_error_sum_sum = temp[0] + temp[1] + temp[2] + temp[3] +
                      temp[4] + temp[5] + temp[6] + temp[7];
    _mm256_storeu_ps(temp, abs_y_ref_sum);
    f32 abs_y_ref_sum_sum = temp[0] + temp[1] + temp[2] + temp[3] +
                      temp[4] + temp[5] + temp[6] + temp[7];

    // Cosine Similarity
    float cosine_similarity = dot_product_sum / (sqrt(y_square_sum_sum) * sqrt(y_ref_square_sum_sum));
    // Relative L1
    float relative_l1 = abs_error_sum_sum / abs_y_ref_sum_sum;
    // RMSE
    float rmse = sqrt(error_square_sum_sum / y.size());
    // Relative L2
    float relative_l2 = sqrt(error_square_sum_sum / y_ref_square_sum_sum);
    metrics.CosineSimilarity = cosine_similarity;
    metrics.RelativeL1 = relative_l1;
    metrics.RMSE = rmse;
    metrics.RelativeL2 = relative_l2;
    return metrics;
}

/// \brief print the error metrics
/// \param metrics the error metrics
inline void print_error_metrics(error_metrics metrics){
    header_print("info", "Cosine Similarity: " << metrics.CosineSimilarity);
    header_print("info", "Relative L1      : " << metrics.RelativeL1);
    header_print("info", "RMSE             : " << metrics.RMSE);
    header_print("info", "Relative L2      : " << metrics.RelativeL2);
}
