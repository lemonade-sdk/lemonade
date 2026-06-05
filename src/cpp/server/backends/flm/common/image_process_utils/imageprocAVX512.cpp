
#include  "image_process_utils/imageproc.hpp"
#include  "image_process_utils/imageprocAVX512.hpp"
#include <omp.h>  // OpenMP for multi-threading

namespace imgproc {
namespace avx512 {

    std::vector<float> resize_bicubic_plane_avx512(
        const std::vector<float>& src,
        int src_w, int src_h,
        int dst_w, int dst_h,
        bool antialias)
    {
        // Fallback to scalar implementation if AVX-512 not available
        if (!has_avx512f()) {
            return resize_bicubic_plane(src, src_w, src_h, dst_w, dst_h, antialias);
        }

        std::vector<float> dst(dst_w * dst_h);

        // PyTorch's exact scale calculation (align_corners=False for resize)
        const float scale_w = area_pixel_compute_scale(src_w, dst_w, false);
        const float scale_h = area_pixel_compute_scale(src_h, dst_h, false);

        // PyTorch antialias logic
        const bool do_antialias_w = antialias && (scale_w > 1.0f);
        const bool do_antialias_h = antialias && (scale_h > 1.0f);

        // PyTorch kernel support and sigma calculation
        const float support = 2.0f; // bicubic kernel support
        const float clamped_scale_w = do_antialias_w ? scale_w : 1.0f;
        const float clamped_scale_h = do_antialias_h ? scale_h : 1.0f;

        // PyTorch's exact sigma calculation
        const float sigma_w = do_antialias_w ? (clamped_scale_w * support) / 2.0f : 0.0f;
        const float sigma_h = do_antialias_h ? (clamped_scale_h * support) / 2.0f : 0.0f;

        // Vectorized constants
        const __m512 v_clamped_scale_w = _mm512_set1_ps(clamped_scale_w);
        const __m512 v_clamped_scale_h = _mm512_set1_ps(clamped_scale_h);
        const __m512 v_sigma_w = _mm512_set1_ps(sigma_w);
        const __m512 v_sigma_h = _mm512_set1_ps(sigma_h);

        for (int dst_y = 0; dst_y < dst_h; ++dst_y) {
            // PyTorch's exact coordinate calculation
            float real_y = area_pixel_compute_source_index(scale_h, dst_y, false, true);
            int input_y = static_cast<int>(std::floor(real_y));

            // Pre-compute y-direction weights for this row
            alignas(64) float y_weights[4];
            alignas(64) float y_coords[4];

            for (int yy = 0; yy < 4; ++yy) {
                y_coords[yy] = real_y - (input_y + yy - 1);
            }

            // Vectorized y-weight computation
            __m128 v_y_coords = _mm_loadu_ps(y_coords);
            __m512 v_y_coords_512 = _mm512_castps128_ps512(v_y_coords);
            v_y_coords_512 = _mm512_insertf32x4(v_y_coords_512, v_y_coords, 0);

            __m512 v_dy = _mm512_div_ps(v_y_coords_512, v_clamped_scale_h);
            __m512 v_wy = bicubic_kernel_avx512(v_dy);

            if (do_antialias_h) {
                __m512 v_gauss = gaussian_avx512(v_y_coords_512, v_sigma_h);
                v_wy = _mm512_mul_ps(v_wy, v_gauss);
            }

            _mm_store_ps(y_weights, _mm512_castps512_ps128(v_wy));

            // Process x-direction with vectorization where possible
            int dst_x = 0;
            const int simd_width = 16;
            const int vectorized_width = (dst_w / simd_width) * simd_width;

            // Vectorized x processing (process multiple destination pixels at once)
            for (; dst_x < vectorized_width; dst_x += simd_width) {
                alignas(64) float dst_pixels[16] = {0};

                for (int vec_offset = 0; vec_offset < simd_width; ++vec_offset) {
                    float real_x = area_pixel_compute_source_index(scale_w, dst_x + vec_offset, false, true);
                    int input_x = static_cast<int>(std::floor(real_x));

                    float sum = 0.0f, wsum = 0.0f;

                    // Y direction loop
                    for (int yy = -1; yy <= 2; ++yy) {
                        int src_y = clamp(input_y + yy, 0, src_h - 1);
                        float wy = y_weights[yy + 1];

                        // X direction - vectorized weight computation
                        alignas(64) float x_coords[4];
                        for (int xx = 0; xx < 4; ++xx) {
                            x_coords[xx] = real_x - (input_x + xx - 1);
                        }

                        __m128 v_x_coords = _mm_loadu_ps(x_coords);
                        __m512 v_x_coords_512 = _mm512_castps128_ps512(v_x_coords);
                        v_x_coords_512 = _mm512_insertf32x4(v_x_coords_512, v_x_coords, 0);

                        __m512 v_dx = _mm512_div_ps(v_x_coords_512, v_clamped_scale_w);
                        __m512 v_wx = bicubic_kernel_avx512(v_dx);

                        if (do_antialias_w) {
                            __m512 v_gauss = gaussian_avx512(v_x_coords_512, v_sigma_w);
                            v_wx = _mm512_mul_ps(v_wx, v_gauss);
                        }

                        alignas(64) float x_weights[4];
                        _mm_store_ps(x_weights, _mm512_castps512_ps128(v_wx));

                        // Accumulate contributions
                        for (int xx = -1; xx <= 2; ++xx) {
                            int src_x = clamp(input_x + xx, 0, src_w - 1);
                            float wx = x_weights[xx + 1];
                            float weight = wx * wy;
                            sum += src[src_y * src_w + src_x] * weight;
                            wsum += weight;
                        }
                    }

                    dst_pixels[vec_offset] = (wsum != 0.0f) ? (sum / wsum) : 0.0f;
                }

                // Store vectorized results
                for (int i = 0; i < simd_width; ++i) {
                    dst[dst_y * dst_w + dst_x + i] = dst_pixels[i];
                }
            }

            // Handle remaining pixels with scalar code
            for (; dst_x < dst_w; ++dst_x) {
                float real_x = area_pixel_compute_source_index(scale_w, dst_x, false, true);
                int input_x = static_cast<int>(std::floor(real_x));

                float sum = 0.0f, wsum = 0.0f;

                // PyTorch uses exact 4x4 neighborhood for bicubic
                for (int yy = -1; yy <= 2; ++yy) {
                    int src_y = clamp(input_y + yy, 0, src_h - 1);
                    float wy = y_weights[yy + 1];

                    for (int xx = -1; xx <= 2; ++xx) {
                        int src_x = clamp(input_x + xx, 0, src_w - 1);
                        float dx = real_x - (input_x + xx);
                        float wx = bicubic_kernel(dx / clamped_scale_w);
                        if (do_antialias_w) {
                            wx *= gaussian(dx, sigma_w);
                        }

                        float weight = wx * wy;
                        sum += src[src_y * src_w + src_x] * weight;
                        wsum += weight;
                    }
                }

                dst[dst_y * dst_w + dst_x] = (wsum != 0.0f) ? (sum / wsum) : 0.0f;
            }
        }
        return dst;
    }

    // AVX-512 optimized main RGB planar entry point
    std::vector<uint8_t> resize_bicubic_antialias_rgb_planar_avx512(
        const uint8_t* src,
        int src_w, int src_h,
        int dst_w, int dst_h,
        bool antialias)
    {
        // Fallback to scalar implementation if AVX-512 not available
        if (!has_avx512f()) {
            return resize_bicubic_antialias_rgb_planar(src, src_w, src_h, dst_w, dst_h, antialias);
        }

        const size_t src_plane_size = static_cast<size_t>(src_w) * src_h;
        const size_t dst_plane_size = static_cast<size_t>(dst_w) * dst_h;

        std::vector<uint8_t> dst(dst_plane_size * 3);

        // Convert to float per plane using AVX-512 optimized conversion
        std::vector<float> r(src_plane_size), g(src_plane_size), b(src_plane_size);

        // Parallelize the uint8 to float conversion (OpenMP for 3 channels)
        #pragma omp parallel sections num_threads(3)
        {
            #pragma omp section
            {
                convert_uint8_to_float_avx512(src, r.data(), src_plane_size);
            }
            #pragma omp section
            {
                convert_uint8_to_float_avx512(src + src_plane_size, g.data(), src_plane_size);
            }
            #pragma omp section
            {
                convert_uint8_to_float_avx512(src + 2 * src_plane_size, b.data(), src_plane_size);
            }
        }

        // Resize each plane using AVX-512 optimized bicubic (parallel processing of RGB channels)
        std::vector<float> r_resized, g_resized, b_resized;

        #pragma omp parallel sections num_threads(3)
        {
            #pragma omp section
            {
                r_resized = resize_bicubic_plane_avx512(r, src_w, src_h, dst_w, dst_h, antialias);
            }
            #pragma omp section
            {
                g_resized = resize_bicubic_plane_avx512(g, src_w, src_h, dst_w, dst_h, antialias);
            }
            #pragma omp section
            {
                b_resized = resize_bicubic_plane_avx512(b, src_w, src_h, dst_w, dst_h, antialias);
            }
        }

        // Convert back to uint8 and store planar using AVX-512 optimized conversion (parallel)
        #pragma omp parallel sections num_threads(3)
        {
            #pragma omp section
            {
                convert_float_to_uint8_avx512(r_resized.data(), dst.data(), dst_plane_size);
            }
            #pragma omp section
            {
                convert_float_to_uint8_avx512(g_resized.data(), dst.data() + dst_plane_size, dst_plane_size);
            }
            #pragma omp section
            {
                convert_float_to_uint8_avx512(b_resized.data(), dst.data() + 2 * dst_plane_size, dst_plane_size);
            }
        }

        return dst;
    }

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
    ) {
        // Fallback to scalar implementation if AVX-512 not available
        if (!has_avx512f()) {
            rescale_and_normalize(image_src, output_buffer, image_width, image_height,
                                image_channels, do_rescale, rescale_factor,
                                do_normalize, image_mean, image_std);
            return;
        }

        // Apply the _fuse_mean_and_rescale_factor optimization
        if (do_rescale && do_normalize) {
            image_mean *= 1.0f / rescale_factor;
            image_std *= 1.0f / rescale_factor;
            do_rescale = false;
        }

        const size_t total_pixels = static_cast<size_t>(image_width) * image_height * image_channels;
        const size_t simd_width = 16; // AVX-512 processes 16 floats at once
        const size_t unroll_factor = 4; // Process 64 elements per iteration
        const size_t vectorized_count = (total_pixels / (simd_width * unroll_factor)) * (simd_width * unroll_factor);

        if (do_normalize) {
            // Vectorized normalization: (pixel - mean) / std
            const __m512 v_mean = _mm512_set1_ps(image_mean);
            const __m512 v_std_inv = _mm512_set1_ps(1.0f / image_std);

            size_t i = 0;

            // Process 64 pixels at a time (4x unrolled)
            for (; i < vectorized_count; i += simd_width * unroll_factor) {
                // Prefetch next cache lines
                _mm_prefetch(reinterpret_cast<const char*>(image_src + i + 64), _MM_HINT_T0);

                // Load 4x16 uint8 values
                __m128i u8_vec0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(image_src + i));
                __m128i u8_vec1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(image_src + i + 16));
                __m128i u8_vec2 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(image_src + i + 32));
                __m128i u8_vec3 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(image_src + i + 48));

                // Convert to 32-bit integers and then to float
                __m512i i32_vec0 = _mm512_cvtepu8_epi32(u8_vec0);
                __m512i i32_vec1 = _mm512_cvtepu8_epi32(u8_vec1);
                __m512i i32_vec2 = _mm512_cvtepu8_epi32(u8_vec2);
                __m512i i32_vec3 = _mm512_cvtepu8_epi32(u8_vec3);

                __m512 f32_vec0 = _mm512_cvtepi32_ps(i32_vec0);
                __m512 f32_vec1 = _mm512_cvtepi32_ps(i32_vec1);
                __m512 f32_vec2 = _mm512_cvtepi32_ps(i32_vec2);
                __m512 f32_vec3 = _mm512_cvtepi32_ps(i32_vec3);

                // Apply normalization: (pixel - mean) / std using FMA
                __m512 normalized0 = _mm512_mul_ps(_mm512_sub_ps(f32_vec0, v_mean), v_std_inv);
                __m512 normalized1 = _mm512_mul_ps(_mm512_sub_ps(f32_vec1, v_mean), v_std_inv);
                __m512 normalized2 = _mm512_mul_ps(_mm512_sub_ps(f32_vec2, v_mean), v_std_inv);
                __m512 normalized3 = _mm512_mul_ps(_mm512_sub_ps(f32_vec3, v_mean), v_std_inv);

                // Store results
                _mm512_storeu_ps(output_buffer + i, normalized0);
                _mm512_storeu_ps(output_buffer + i + 16, normalized1);
                _mm512_storeu_ps(output_buffer + i + 32, normalized2);
                _mm512_storeu_ps(output_buffer + i + 48, normalized3);
            }

            // Process remaining 16-element chunks
            for (; i + simd_width <= total_pixels; i += simd_width) {
                __m128i u8_vec = _mm_loadu_si128(reinterpret_cast<const __m128i*>(image_src + i));
                __m512i i32_vec = _mm512_cvtepu8_epi32(u8_vec);
                __m512 f32_vec = _mm512_cvtepi32_ps(i32_vec);
                __m512 normalized = _mm512_mul_ps(_mm512_sub_ps(f32_vec, v_mean), v_std_inv);
                _mm512_storeu_ps(output_buffer + i, normalized);
            }

            // Handle remaining pixels with scalar code
            for (; i < total_pixels; ++i) {
                output_buffer[i] = (static_cast<float>(image_src[i]) - image_mean) / image_std;
            }

        } else if (do_rescale) {
            // Vectorized rescaling: pixel * rescale_factor
            const __m512 v_rescale = _mm512_set1_ps(rescale_factor);

            size_t i = 0;

            // Process 64 pixels at a time (4x unrolled)
            for (; i < vectorized_count; i += simd_width * unroll_factor) {
                // Prefetch next cache lines
                _mm_prefetch(reinterpret_cast<const char*>(image_src + i + 64), _MM_HINT_T0);

                // Load 4x16 uint8 values
                __m128i u8_vec0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(image_src + i));
                __m128i u8_vec1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(image_src + i + 16));
                __m128i u8_vec2 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(image_src + i + 32));
                __m128i u8_vec3 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(image_src + i + 48));

                // Convert to 32-bit integers and then to float
                __m512i i32_vec0 = _mm512_cvtepu8_epi32(u8_vec0);
                __m512i i32_vec1 = _mm512_cvtepu8_epi32(u8_vec1);
                __m512i i32_vec2 = _mm512_cvtepu8_epi32(u8_vec2);
                __m512i i32_vec3 = _mm512_cvtepu8_epi32(u8_vec3);

                __m512 f32_vec0 = _mm512_cvtepi32_ps(i32_vec0);
                __m512 f32_vec1 = _mm512_cvtepi32_ps(i32_vec1);
                __m512 f32_vec2 = _mm512_cvtepi32_ps(i32_vec2);
                __m512 f32_vec3 = _mm512_cvtepi32_ps(i32_vec3);

                // Apply rescaling: pixel * rescale_factor
                __m512 rescaled0 = _mm512_mul_ps(f32_vec0, v_rescale);
                __m512 rescaled1 = _mm512_mul_ps(f32_vec1, v_rescale);
                __m512 rescaled2 = _mm512_mul_ps(f32_vec2, v_rescale);
                __m512 rescaled3 = _mm512_mul_ps(f32_vec3, v_rescale);

                // Store results
                _mm512_storeu_ps(output_buffer + i, rescaled0);
                _mm512_storeu_ps(output_buffer + i + 16, rescaled1);
                _mm512_storeu_ps(output_buffer + i + 32, rescaled2);
                _mm512_storeu_ps(output_buffer + i + 48, rescaled3);
            }

            // Process remaining 16-element chunks
            for (; i + simd_width <= total_pixels; i += simd_width) {
                __m128i u8_vec = _mm_loadu_si128(reinterpret_cast<const __m128i*>(image_src + i));
                __m512i i32_vec = _mm512_cvtepu8_epi32(u8_vec);
                __m512 f32_vec = _mm512_cvtepi32_ps(i32_vec);
                __m512 rescaled = _mm512_mul_ps(f32_vec, v_rescale);
                _mm512_storeu_ps(output_buffer + i, rescaled);
            }

            // Handle remaining pixels with scalar code
            for (; i < total_pixels; ++i) {
                output_buffer[i] = static_cast<float>(image_src[i]) * rescale_factor;
            }

        } else {
            // Just convert uint8 to float (no rescaling or normalization)
            convert_uint8_to_float_avx512(image_src, output_buffer, total_pixels);
        }
    }

    void rescale_and_normalize_avx512(
        const uint8_t *image_src,
        float *output_buffer,
        int image_width, int image_height, int image_channels,
        bool do_rescale,
        float rescale_factor,
        bool do_normalize,
        const std::vector<float>& image_mean,
        const std::vector<float>& image_std
    ) {
        if (!has_avx512f()) {
            // Fallback to scalar implementation
            size_t plane_size = static_cast<size_t>(image_width) * image_height;
            int channels = std::min(image_channels, 3);

            for (int c = 0; c < channels; ++c) {
                const uint8_t* p_src = image_src + c * plane_size;
                float* p_dst = output_buffer + c * plane_size;
                float mean = (do_normalize && c < image_mean.size()) ? image_mean[c] : 0.0f;
                float std_dev = (do_normalize && c < image_std.size()) ? image_std[c] : 1.0f;

                bool local_rescale = do_rescale;
                if (local_rescale && do_normalize) {
                    mean *= (1.0f / rescale_factor);
                    std_dev *= (1.0f / rescale_factor);
                    local_rescale = false;
                }

                float std_inv = (std_dev != 0.0f) ? 1.0f / std_dev : 0.0f;

                for (size_t i = 0; i < plane_size; ++i) {
                    float val = static_cast<float>(p_src[i]);
                    if (local_rescale) val *= rescale_factor;
                    if (do_normalize) {
                         val = (val - mean) * std_inv;
                    }
                    p_dst[i] = val;
                }
            }
            return;
        }

        const size_t plane_size = static_cast<size_t>(image_width) * image_height;
        const int channels = std::min(image_channels, 3);

        #pragma omp parallel for
        for (int c = 0; c < channels; ++c) {
            const uint8_t* p_src = image_src + c * plane_size;
            float* p_dst = output_buffer + c * plane_size;

            float mean = (c < image_mean.size()) ? image_mean[c] : 0.0f;
            float std_dev = (c < image_std.size()) ? image_std[c] : 1.0f;

            bool local_do_rescale = do_rescale;

            if (local_do_rescale && do_normalize) {
                mean *= 1.0f / rescale_factor;
                std_dev *= 1.0f / rescale_factor;
                local_do_rescale = false;
            }

            const size_t total_pixels = plane_size;
            const size_t simd_width = 16;
            const size_t unroll_factor = 4;
            const size_t vectorized_count = (total_pixels / (simd_width * unroll_factor)) * (simd_width * unroll_factor);

            size_t i = 0;

            if (do_normalize) {
                const __m512 v_mean = _mm512_set1_ps(mean);
                const __m512 v_std_inv = _mm512_set1_ps(1.0f / std_dev);

                for (; i < vectorized_count; i += simd_width * unroll_factor) {
                    _mm_prefetch(reinterpret_cast<const char*>(p_src + i + 64), _MM_HINT_T0);

                    __m128i u8_vec0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p_src + i));
                    __m128i u8_vec1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p_src + i + 16));
                    __m128i u8_vec2 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p_src + i + 32));
                    __m128i u8_vec3 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p_src + i + 48));

                    __m512 f32_vec0 = _mm512_cvtepi32_ps(_mm512_cvtepu8_epi32(u8_vec0));
                    __m512 f32_vec1 = _mm512_cvtepi32_ps(_mm512_cvtepu8_epi32(u8_vec1));
                    __m512 f32_vec2 = _mm512_cvtepi32_ps(_mm512_cvtepu8_epi32(u8_vec2));
                    __m512 f32_vec3 = _mm512_cvtepi32_ps(_mm512_cvtepu8_epi32(u8_vec3));

                    _mm512_storeu_ps(p_dst + i, _mm512_mul_ps(_mm512_sub_ps(f32_vec0, v_mean), v_std_inv));
                    _mm512_storeu_ps(p_dst + i + 16, _mm512_mul_ps(_mm512_sub_ps(f32_vec1, v_mean), v_std_inv));
                    _mm512_storeu_ps(p_dst + i + 32, _mm512_mul_ps(_mm512_sub_ps(f32_vec2, v_mean), v_std_inv));
                    _mm512_storeu_ps(p_dst + i + 48, _mm512_mul_ps(_mm512_sub_ps(f32_vec3, v_mean), v_std_inv));
                }

                for (; i + simd_width <= total_pixels; i += simd_width) {
                    __m512 f32_vec = _mm512_cvtepi32_ps(_mm512_cvtepu8_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p_src + i))));
                    _mm512_storeu_ps(p_dst + i, _mm512_mul_ps(_mm512_sub_ps(f32_vec, v_mean), v_std_inv));
                }

                for (; i < total_pixels; ++i) {
                    p_dst[i] = (static_cast<float>(p_src[i]) - mean) / std_dev;
                }
            } else if (local_do_rescale) {
                const __m512 v_rescale = _mm512_set1_ps(rescale_factor);
                for (; i < vectorized_count; i += simd_width * unroll_factor) {
                    _mm_prefetch(reinterpret_cast<const char*>(p_src + i + 64), _MM_HINT_T0);

                    __m512 f32_vec0 = _mm512_cvtepi32_ps(_mm512_cvtepu8_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p_src + i))));
                    __m512 f32_vec1 = _mm512_cvtepi32_ps(_mm512_cvtepu8_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p_src + i + 16))));
                    __m512 f32_vec2 = _mm512_cvtepi32_ps(_mm512_cvtepu8_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p_src + i + 32))));
                    __m512 f32_vec3 = _mm512_cvtepi32_ps(_mm512_cvtepu8_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p_src + i + 48))));

                    _mm512_storeu_ps(p_dst + i, _mm512_mul_ps(f32_vec0, v_rescale));
                    _mm512_storeu_ps(p_dst + i + 16, _mm512_mul_ps(f32_vec1, v_rescale));
                    _mm512_storeu_ps(p_dst + i + 32, _mm512_mul_ps(f32_vec2, v_rescale));
                    _mm512_storeu_ps(p_dst + i + 48, _mm512_mul_ps(f32_vec3, v_rescale));
                }
                for (; i + simd_width <= total_pixels; i += simd_width) {
                    __m512 f32_vec = _mm512_cvtepi32_ps(_mm512_cvtepu8_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p_src + i))));
                    _mm512_storeu_ps(p_dst + i, _mm512_mul_ps(f32_vec, v_rescale));
                }
                for (; i < total_pixels; ++i) {
                    p_dst[i] = static_cast<float>(p_src[i]) * rescale_factor;
                }
            } else {
                convert_uint8_to_float_avx512(p_src, p_dst, total_pixels);
            }
        }
    }

} // namespace avx512
} // namespace imgproc
