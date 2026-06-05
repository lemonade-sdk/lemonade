/// \file modeling_Qwen2VL_image.cpp
/// \brief Qwen2VL image processing implementation
/// \author FastFlowLM Team
/// \date 2025-09-01
/// \version 0.9.24
/// \note This is a source file for the Qwen2VL image processing functionality

#include "AutoModel/modeling_qwen2vl.hpp"

qwen2vl_image_t Qwen2VL::load_image(const std::string& filename) {
    qwen2vl_image_t empty_result;
    image_data_t decoded;
    image_data_t reordered;
    if (!image_reader_.load_image(filename, decoded)) {
        return empty_result;
    }

    if (this->image_pre_resize > 0) {
        int max_height;
        switch(this->image_pre_resize) {
            case 1:
                max_height = 480;
                break;
            case 2:
                max_height = 720;
                break;
            case 3:
                max_height = 1080;
                break;
            case 4:
                max_height = 1440;
                break;
            default:
                max_height = decoded.height; // no resizing
                break;
        }

        if (decoded.height > max_height) {
            image_data_t resized_image;
            float ratio = static_cast<float>(max_height) / static_cast<float>(decoded.height);
            int target_width = static_cast<int>(static_cast<float>(decoded.width) * ratio);
            int target_height = max_height;
            header_print_r("FLM", "Qwen2VL resizing image from (" + std::to_string(decoded.width) + ", " + std::to_string(decoded.height) + ") to (" + std::to_string(target_width) + ", " + std::to_string(target_height) + ")\n");
            if (image_reader_.resize_image(decoded, target_width, target_height, resized_image)) {
                image_reader_.recycle(decoded);
                decoded = std::move(resized_image);
            }
        }
    }

    if (!image_reader_.reorder_hwc_to_chw(decoded, reordered)) {
        image_reader_.recycle(decoded);
        return empty_result;
    }

    image_reader_.recycle(decoded);

    qwen2vl_image_t result;
    result.width = reordered.width;
    result.height = reordered.height;
    result._data = std::move(reordered.pixels);
    reordered.width = 0;
    reordered.height = 0;
    return result;
}

qwen2vl_image_t Qwen2VL::load_image_base64(const std::string& base64_string) {
    qwen2vl_image_t empty_result;
    image_data_t decoded;
    image_data_t reordered;
    if (!image_reader_.load_image_base64(base64_string, decoded)) {
        return empty_result;
    }

    if (this->image_pre_resize > 0) {
        int max_height;
        switch(this->image_pre_resize) {
            case 1:
                max_height = 480;
                break;
            case 2:
                max_height = 720;
                break;
            case 3:
                max_height = 1080;
                break;
            case 4:
                max_height = 1440;
                break;
            default:
                max_height = decoded.height; // no resizing
                break;
        }

        if (decoded.height > max_height) {
            image_data_t resized_image;
            float ratio = static_cast<float>(max_height) / static_cast<float>(decoded.height);
            int target_width = static_cast<int>(static_cast<float>(decoded.width) * ratio);
            int target_height = max_height;
            header_print_r("FLM", "Qwen2VL resizing image from (" + std::to_string(decoded.width) + ", " + std::to_string(decoded.height) + ") to (" + std::to_string(target_width) + ", " + std::to_string(target_height) + ")");
            if (image_reader_.resize_image(decoded, target_width, target_height, resized_image)) {
                image_reader_.recycle(decoded);
                decoded = std::move(resized_image);
            }
        }
    }

    if (!image_reader_.reorder_hwc_to_chw(decoded, reordered)) {
        image_reader_.recycle(decoded);
        return empty_result;
    }

    image_reader_.recycle(decoded);

    qwen2vl_image_t result;
    result.width = reordered.width;
    result.height = reordered.height;
    result._data = std::move(reordered.pixels);
    reordered.width = 0;
    reordered.height = 0;
    return result;
}


void Qwen2VL::smart_resize(
    int height,
    int width,
    int& h_bar,
    int& w_bar,
    int factor,
    int min_pixels,
    int max_pixels
) {
    double aspect_ratio = static_cast<double>(std::max(height, width)) /
                          static_cast<double>(std::min(height, width));
    if (aspect_ratio > 200.0) {
        std::cerr << "absolute aspect ratio must be smaller than 200, got " +
            std::to_string(aspect_ratio);
    }

    h_bar = static_cast<int>(std::round(static_cast<double>(height) / factor)) * factor;
    w_bar = static_cast<int>(std::round(static_cast<double>(width) / factor)) * factor;

    long long total_pixels = static_cast<long long>(h_bar) * w_bar;

    if (total_pixels > max_pixels) {
        double beta = std::sqrt((static_cast<double>(height) * width) / max_pixels);
        h_bar = std::max(factor,
                         static_cast<int>(std::floor(height / beta / factor)) * factor);
        w_bar = std::max(factor,
                         static_cast<int>(std::floor(width / beta / factor)) * factor);
    } else if (total_pixels < min_pixels) {
        double beta = std::sqrt(static_cast<double>(min_pixels) /
                                (static_cast<double>(height) * width));
        h_bar = static_cast<int>(std::ceil(height * beta / factor)) * factor;
        w_bar = static_cast<int>(std::ceil(width * beta / factor)) * factor;
    }
}


///@brief: preprocess the image for Qwen2VL model
///@note: Converts uint8 image to BF16 format, data is already in (3, H, W) CHW layout
///@param: image: the image to preprocess (already in CHW format)
///@return: the preprocessed image in BF16 format
void Qwen2VL::preprocess_image(qwen2vl_image_t& image, std::vector<bf16> &pixel_values) {
    const int width = image.width;
    const int height = image.height;
    const int channels = 3; // RGB
    int resized_height;
    int resized_width;
    // do the automatically resizing in here
    smart_resize(
        height, width,
        resized_height, resized_width,
        QWEN2_PATCH_SIZE * QWEN2_IMAGE_MERGE_SIZE,
        QWEN2_SHORTEST_EDGE,
        QWEN2_LONGEST_EDGE
    );
    // std::cout << "resized_height "<< resized_height << " resized_width " << resized_width <<std::endl;

    // Use non-optimized path for consistent results across platforms
    auto resize_image =  imgproc::avx512::resize_bicubic_antialias_rgb_planar_avx512(
        image._data.data(), width, height, resized_width, resized_height, true
    );

    // Padding Logic (Align to Window Size)
    const int target_window_size = 112;
    const int vit_merger_window_size = target_window_size / QWEN2_IMAGE_MERGE_SIZE / QWEN2_PATCH_SIZE;
    const int align_unit = QWEN2_PATCH_SIZE * QWEN2_IMAGE_MERGE_SIZE * vit_merger_window_size;

    int pad_h = (align_unit - (resized_height % align_unit)) % align_unit;
    int pad_w = (align_unit - (resized_width % align_unit)) % align_unit;

    if (pad_h > 0 || pad_w > 0) {
        const int padded_width = resized_width + pad_w;
        const int padded_height = resized_height + pad_h;

        const size_t src_plane_size = static_cast<size_t>(resized_width) * resized_height;
        const size_t dst_plane_size = static_cast<size_t>(padded_width) * padded_height;

        resize_image.resize(dst_plane_size * channels);

        // Expand each plane in-place from back to front to avoid extra temporary allocations.
        for (int c = channels - 1; c >= 0; --c) {
            uint8_t* src_plane = resize_image.data() + static_cast<size_t>(c) * src_plane_size;
            uint8_t* dst_plane = resize_image.data() + static_cast<size_t>(c) * dst_plane_size;

            for (int y = resized_height - 1; y >= 0; --y) {
                std::memmove(dst_plane + static_cast<size_t>(y) * padded_width,
                    src_plane + static_cast<size_t>(y) * resized_width,
                    resized_width);
                if (pad_w > 0) {
                    std::memset(dst_plane + static_cast<size_t>(y) * padded_width + resized_width, 0, pad_w);
                }
            }

            if (pad_h > 0) {
                std::memset(dst_plane + static_cast<size_t>(resized_height) * padded_width,
                    0,
                    static_cast<size_t>(pad_h) * padded_width);
            }
        }

        resized_width = padded_width;
        resized_height = padded_height;
    }

    // Cache size calculations for efficiency
    const uint32_t single_frame_size = resized_height * resized_width * channels;
    const uint32_t total_patch_size = single_frame_size * QWEN2_TEMPORAL_PATCH_SIZE;
    const uint32_t grid_h = resized_height / QWEN2_PATCH_SIZE;
    const uint32_t grid_w = resized_width / QWEN2_PATCH_SIZE;

    // Pre-allocate final buffer to avoid reallocation
    const uint32_t prev_pixel_values_size = pixel_values.size();
    pixel_values.resize(prev_pixel_values_size + total_patch_size);

    // Reuse scratch buffer across calls to avoid repeated allocations
    static thread_local std::vector<float> patch_vector_scratch;
    if (patch_vector_scratch.size() < total_patch_size) {
        patch_vector_scratch.resize(total_patch_size);
    }

    // Apply rescale and normalization to first frame
    imgproc::avx512::rescale_and_normalize_avx512(
        resize_image.data(), patch_vector_scratch.data(),
        resized_width, resized_height, channels,
        true, QWEN2_VISION_RESCALE_FACTOR,
        true,
        {QWEN2_VISION_RESCALE_IMAGE_MEAN_R, QWEN2_VISION_RESCALE_IMAGE_MEAN_G, QWEN2_VISION_RESCALE_IMAGE_MEAN_B},
        {QWEN2_VISION_RESCALE_IMAGE_STD_R, QWEN2_VISION_RESCALE_IMAGE_STD_G, QWEN2_VISION_RESCALE_IMAGE_STD_B}
    );

    // Replicate first frame for temporal patches (optimized for QWEN2_TEMPORAL_PATCH_SIZE = 2)
    // This is more efficient than a loop for the common case
    if constexpr (QWEN2_TEMPORAL_PATCH_SIZE == 2) {
        memcpy(
            patch_vector_scratch.data() + single_frame_size,
            patch_vector_scratch.data(),
            single_frame_size * sizeof(float)
        );
    } else {
        // Generic loop for other TEMPORAL_PATCH_SIZE values
        for(unsigned l = 1; l < QWEN2_TEMPORAL_PATCH_SIZE; l++){
            memcpy(
                patch_vector_scratch.data() + l * single_frame_size,
                patch_vector_scratch.data(),
                single_frame_size * sizeof(float)
            );
        }
    }

    // Reorder patches directly into pre-allocated pixel_values
    imgproc::reorder_patches_inplace(
        patch_vector_scratch.data(),
        pixel_values.data() + prev_pixel_values_size,
        1, 1, // something special for image
        QWEN2_TEMPORAL_PATCH_SIZE,
        channels,
        grid_h, grid_w,
        QWEN2_MERGE_SIZE,
        QWEN2_PATCH_SIZE
    );


    image.width_resized = resized_width;
    image.height_resized = resized_height;
    image.grid_h = grid_h;
    image.grid_w = grid_w;
    image._data.free(); // free the data

    // release the original uint8_t data now, since is no longer useful to us
}
