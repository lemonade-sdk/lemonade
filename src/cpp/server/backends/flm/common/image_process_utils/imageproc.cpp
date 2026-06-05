#include  "image_process_utils/imageproc.hpp"
#include  "image_process_utils/imageprocAVX512.hpp"
#include <immintrin.h>  // AVX-512 intrinsics
#include <omp.h>         // OpenMP for multi-threading

namespace imgproc {

    std::vector<float> resize_bicubic_plane(
        const std::vector<float>& src,
        int src_w, int src_h,
        int dst_w, int dst_h,
        bool antialias)
    {
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

        for (int dst_y = 0; dst_y < dst_h; ++dst_y) {
            // PyTorch's exact coordinate calculation
            float real_y = area_pixel_compute_source_index(scale_h, dst_y, false, true);
            int input_y = static_cast<int>(std::floor(real_y));
            float t_y = real_y - input_y;

            for (int dst_x = 0; dst_x < dst_w; ++dst_x) {
                float real_x = area_pixel_compute_source_index(scale_w, dst_x, false, true);
                int input_x = static_cast<int>(std::floor(real_x));
                float t_x = real_x - input_x;

                float sum = 0.0f, wsum = 0.0f;

                // PyTorch uses exact 4x4 neighborhood for bicubic
                for (int yy = -1; yy <= 2; ++yy) {
                    int src_y = clamp(input_y + yy, 0, src_h - 1);
                    float dy = real_y - (input_y + yy);
                    float wy = bicubic_kernel(dy / clamped_scale_h);
                    if (do_antialias_h) {
                        wy *= gaussian(dy, sigma_h);
                    }

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

    // Main planar RGB entry point
    std::vector<uint8_t> resize_bicubic_antialias_rgb_planar(
        const uint8_t* src,
        int src_w, int src_h,
        int dst_w, int dst_h,
        bool antialias)
    {
        const size_t src_plane_size = static_cast<size_t>(src_w) * src_h;
        const size_t dst_plane_size = static_cast<size_t>(dst_w) * dst_h;

        std::vector<uint8_t> dst(dst_plane_size * 3);

        // Convert to float per plane
        std::vector<float> r(src_plane_size), g(src_plane_size), b(src_plane_size);
        for (size_t i = 0; i < src_plane_size; ++i) {
            r[i] = static_cast<float>(src[i]);
            g[i] = static_cast<float>(src[src_plane_size + i]);
            b[i] = static_cast<float>(src[2 * src_plane_size + i]);
        }

        // Resize each plane
        auto r_resized = resize_bicubic_plane(r, src_w, src_h, dst_w, dst_h, antialias);
        auto g_resized = resize_bicubic_plane(g, src_w, src_h, dst_w, dst_h, antialias);
        auto b_resized = resize_bicubic_plane(b, src_w, src_h, dst_w, dst_h, antialias);

        // Convert back to uint8 and store planar
        for (size_t i = 0; i < dst_plane_size; ++i) {
            dst[i] = static_cast<uint8_t>(std::clamp(std::round(r_resized[i]), 0.0f, 255.0f));
            dst[dst_plane_size + i] = static_cast<uint8_t>(std::clamp(std::round(g_resized[i]), 0.0f, 255.0f));
            dst[2 * dst_plane_size + i] = static_cast<uint8_t>(std::clamp(std::round(b_resized[i]), 0.0f, 255.0f));
        }

        return dst;
    }

    // Auto-dispatching versions that select best implementation at runtime
    std::vector<float> resize_bicubic_plane_optimized(
        const std::vector<float>& src,
        int src_w, int src_h,
        int dst_w, int dst_h,
        bool antialias)
    {
        if (avx512::has_avx512f()) {
            return avx512::resize_bicubic_plane_avx512(src, src_w, src_h, dst_w, dst_h, antialias);
        } else {
            return resize_bicubic_plane(src, src_w, src_h, dst_w, dst_h, antialias);
        }
    }

    std::vector<uint8_t> resize_bicubic_antialias_rgb_planar_optimized(
        const uint8_t* src,
        int src_w, int src_h,
        int dst_w, int dst_h,
        bool antialias)
    {
        if (avx512::has_avx512f()) {
            return avx512::resize_bicubic_antialias_rgb_planar_avx512(src, src_w, src_h, dst_w, dst_h, antialias);
        } else {
            return resize_bicubic_antialias_rgb_planar(src, src_w, src_h, dst_w, dst_h, antialias);
        }
    }







    void rescale_and_normalize(
        const uint8_t *image_src,
        float *output_buffer,
        int image_width, int image_height, int image_channels,
        bool do_rescale,
        float rescale_factor,
        bool do_normalize,
        float image_mean,
        float image_std
    ){

        // tHE _fuse_mean_and_rescale_factor
        if(do_rescale && do_normalize){
            image_mean *= 1.0f/(rescale_factor);
            image_std *= 1.0f/(rescale_factor);
            do_rescale = false;
        }

        if(do_normalize){
            for(int c = 0; c < image_channels; c++){
                for(int h = 0; h < image_height; h++){
                    for(int w = 0; w < image_width; w++){
                        int idx = c * image_height * image_width + h * image_width + w;
                        output_buffer[idx] = (static_cast<float>(image_src[idx]) - image_mean) / image_std;
                    }
                }
            }
        }else if(do_rescale){
            for(int c = 0; c < image_channels; c++){
                for(int h = 0; h < image_height; h++){
                    for(int w = 0; w < image_width; w++){
                        int idx = c * image_height * image_width + h * image_width + w;
                        output_buffer[idx] = static_cast<float>(image_src[idx]) * rescale_factor;
                    }
                }
            }
        }



    }

    // Auto-dispatching optimized version that selects best implementation at runtime
    void rescale_and_normalize_optimized(
        const uint8_t *image_src,
        float *output_buffer,
        int image_width, int image_height, int image_channels,
        bool do_rescale,
        float rescale_factor,
        bool do_normalize,
        float image_mean,
        float image_std
    ) {
        if (avx512::has_avx512f()) {
            avx512::rescale_and_normalize_avx512(image_src, output_buffer, image_width, image_height,
                                               image_channels, do_rescale, rescale_factor,
                                               do_normalize, image_mean, image_std);
        } else {
            rescale_and_normalize(image_src, output_buffer, image_width, image_height,
                                image_channels, do_rescale, rescale_factor,
                                do_normalize, image_mean, image_std);
        }
    }

    void reorder_patches_inplace(
        float* data,
        bf16* out_ptr,
        int batch_size,
        int grid_t,
        int temporal_patch_size,
        int channel,
        int grid_h,
        int grid_w,
        int merge_size,
        int patch_size)
    {
        assert(grid_h % merge_size == 0);
        assert(grid_w % merge_size == 0);

        int gh_group = grid_h / merge_size;
        int gw_group = grid_w / merge_size;

        // Input tensor strides for the viewed layout:
        // (batch_size, grid_t, temporal_patch_size, channel, grid_h//merge_size, merge_size, patch_size, grid_w//merge_size, merge_size, patch_size)
        size_t in_stride_pw = 1;                                                           // patch_w (dim 9)
        size_t in_stride_mw = patch_size;                                                 // merge_w (dim 8)
        size_t in_stride_gw_grp = merge_size * in_stride_mw;                             // grid_w//merge_size (dim 7)
        size_t in_stride_ph = gw_group * in_stride_gw_grp;                               // patch_h (dim 6)
        size_t in_stride_mh = patch_size * in_stride_ph;                                 // merge_h (dim 5)
        size_t in_stride_gh_grp = merge_size * in_stride_mh;                             // grid_h//merge_size (dim 4)
        size_t in_stride_ch = gh_group * in_stride_gh_grp;                               // channel (dim 3)
        size_t in_stride_tp = channel * in_stride_ch;                                    // temporal_patch_size (dim 2)
        size_t in_stride_gt = temporal_patch_size * in_stride_tp;                        // grid_t (dim 1)
        size_t in_stride_b = grid_t * in_stride_gt;                                      // batch (dim 0)

        // Calculate total number of outer iterations for parallelization
        int total_outer_iters = batch_size * grid_t * gh_group * gw_group * merge_size * merge_size;

        // Parallelize over the outer dimensions with OpenMP (max 4 threads)
        // Each thread processes different spatial/temporal patches
        #pragma omp parallel for num_threads(4) schedule(dynamic, 4)
        for (int outer_idx = 0; outer_idx < total_outer_iters; ++outer_idx) {
            // Decompose outer_idx back into individual indices
            int temp = outer_idx;
            int mw = temp % merge_size; temp /= merge_size;
            int mh = temp % merge_size; temp /= merge_size;
            int gw_grp = temp % gw_group; temp /= gw_group;
            int gh_grp = temp % gh_group; temp /= gh_group;
            int gt = temp % grid_t; temp /= grid_t;
            int b = temp;

            // Pre-calculate base indices for outer loops
            size_t base_idx = b * in_stride_b +
                            gt * in_stride_gt +
                            gh_grp * in_stride_gh_grp +
                            gw_grp * in_stride_gw_grp +
                            mh * in_stride_mh +
                            mw * in_stride_mw;

            // Calculate output base position
            size_t out_base = (size_t)outer_idx * channel * temporal_patch_size * patch_size * patch_size;

            // Inner loops over channel, temporal_patch_size, patch_h, patch_w
            for (int c = 0; c < channel; ++c) {
                size_t c_base = base_idx + c * in_stride_ch;

                for (int tp = 0; tp < temporal_patch_size; ++tp) {
                    size_t tp_base = c_base + tp * in_stride_tp;

                    for (int ph = 0; ph < patch_size; ++ph) {
                        size_t ph_base = tp_base + ph * in_stride_ph;

                        // Innermost loop: patch_w dimension
                        // Use AVX-512 SIMD for vectorized conversion when patch_size=16
                        int pw = 0;

                        #if defined(__AVX512F__)
                        // Process 16 elements at once if patch_size == 16
                        if (patch_size == 16) {
                            size_t in_idx = ph_base;
                            size_t out_idx = out_base +
                                           (c * temporal_patch_size * patch_size * patch_size) +
                                           (tp * patch_size * patch_size) +
                                           (ph * patch_size);

                            // Load 16 floats
                            __m512 float_vec = _mm512_loadu_ps(&data[in_idx]);

                            // Convert to bfloat16 with RNE (round to nearest even)
                            __m512i int_data = _mm512_castps_si512(float_vec);
                            __m512i rounding = _mm512_set1_epi32(0x7FFF);
                            __m512i rounded = _mm512_add_epi32(int_data, rounding);
                            __m512i shifted = _mm512_srli_epi32(rounded, 16);
                            __m256i bf16_data = _mm512_cvtepi32_epi16(shifted);

                            // Store 16 bfloat16 values
                            _mm256_storeu_si256(reinterpret_cast<__m256i*>(&out_ptr[out_idx]), bf16_data);

                            pw = patch_size; // Skip scalar loop
                        }
                        #endif

                        // Scalar fallback for other patch sizes or non-AVX512 systems
                        for (; pw < patch_size; ++pw) {
                            size_t in_idx = ph_base + pw * in_stride_pw;
                            size_t out_idx = out_base +
                                           (c * temporal_patch_size * patch_size * patch_size) +
                                           (tp * patch_size * patch_size) +
                                           (ph * patch_size) +
                                           pw;

                            out_ptr[out_idx] = bf16(data[in_idx]);
                        }
                    }
                }
            }
        }
    }
} // namespace imgproc
