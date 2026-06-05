#pragma once

#include <vector>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <cassert>
#include <cstring>
#include "typedef.hpp"
namespace imgproc {

    // Keys bicubic kernel (a = -0.5) - matches PyTorch antialias=True
    inline float bicubic_kernel(float x) {
        const float a = -0.5f;
        x = std::fabs(x);
        if (x < 1.0f)
            return ((a + 2.0f) * x - (a + 3.0f)) * x * x + 1.0f;
        else if (x < 2.0f)
            return ((a * x - 5.0f * a) * x + 8.0f * a) * x - 4.0f * a;
        else
            return 0.0f;
    }

    inline int clamp(int x, int lo, int hi) {
        return std::max(lo, std::min(x, hi));
    }

    inline float gaussian(float x, float sigma) {
        if (sigma <= 0.0f) return 1.0f;
        return std::exp(-(x * x) / (2.0f * sigma * sigma));
    }

    // PyTorch-compatible area pixel compute scale
    inline float area_pixel_compute_scale(int input_size, int output_size, bool align_corners) {
        if (align_corners) {
            if (output_size > 1) {
                return static_cast<float>(input_size - 1) / (output_size - 1);
            } else {
                return 0.0f;
            }
        } else {
            return static_cast<float>(input_size) / output_size;
        }
    }

    // PyTorch-compatible area pixel compute source index
    inline float area_pixel_compute_source_index(float scale, int dst_index, bool align_corners, bool cubic) {
        if (align_corners) {
            return scale * dst_index;
        } else {
            float src_idx = scale * (dst_index + 0.5f) - 0.5f;
            return cubic ? src_idx : std::max(src_idx, 0.0f);
        }
    }

    // Core bicubic resize with exact PyTorch antialiasing behavior
    std::vector<float> resize_bicubic_plane(
        const std::vector<float>& src,
        int src_w, int src_h,
        int dst_w, int dst_h,
        bool antialias);

    // Main planar RGB entry point
    std::vector<uint8_t> resize_bicubic_antialias_rgb_planar(
        const uint8_t* src,
        int src_w, int src_h,
        int dst_w, int dst_h,
        bool antialias);

    // Auto-dispatching versions that select best implementation at runtime
    std::vector<float> resize_bicubic_plane_optimized(
        const std::vector<float>& src,
        int src_w, int src_h,
        int dst_w, int dst_h,
        bool antialias);

    std::vector<uint8_t> resize_bicubic_antialias_rgb_planar_optimized(
        const uint8_t* src,
        int src_w, int src_h,
        int dst_w, int dst_h,
        bool antialias);


    void rescale_and_normalize(
        const uint8_t *image_src,
        float *output_buffer,
        int image_width, int image_height, int image_channels,
        bool do_rescale,
        float rescale_factor,
        bool do_normalize,
        float image_mean,  // Assume image_mean and image_std are the same for all channels
        float image_std
    );

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
    );



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
        int patch_size);


} // namespace imgproc
