/// \file modeling_gemma3_image.cpp
/// \brief Gemma3 image processing implementation
/// \author FastFlowLM Team
/// \date 2025-09-01
/// \version 0.9.24
/// \note This is a source file for the Gemma3 image processing functionality

#include "AutoModel/modeling_gemma3.hpp"

bytes Gemma3::load_image(const std::string& filename) {
    constexpr int target_width = 896;
    constexpr int target_height = 896;

    image_data_t decoded;
    image_data_t resized;

    if (!image_reader_.load_image(filename, decoded)) {
        return bytes();
    }

    if (!image_reader_.resize_image(decoded, target_width, target_height, resized)) {
        image_reader_.recycle(decoded);
        return bytes();
    }

    image_reader_.recycle(decoded);
    bytes result = std::move(resized.pixels);
    resized.width = 0;
    resized.height = 0;
    return result;
}

bytes Gemma3::load_image_base64(const std::string& base64_string) {
    constexpr int target_width = 896;
    constexpr int target_height = 896;

    image_data_t decoded;
    image_data_t resized;

    if (!image_reader_.load_image_base64(base64_string, decoded)) {
        return bytes();
    }

    if (!image_reader_.resize_image(decoded, target_width, target_height, resized)) {
        image_reader_.recycle(decoded);
        return bytes();
    }

    image_reader_.recycle(decoded);
    bytes result = std::move(resized.pixels);
    resized.width = 0;
    resized.height = 0;
    return result;
}

///@brief: preprocess the image for gemma3 model
///@note: 1. Reorder: 896x896x3 -> 3x896x896
///@param: image: the image to preprocess
///@return: the preprocessed image
buffer<bf16> Gemma3::preprocess_image(bytes& image) {
    buffer<bf16> result(3 * 896 * 896);
    const int total_pixels = 896 * 896;
    image_data_t input;
    input.width = 896;
    input.height = 896;
    input.pixels = image;

    image_data_t chw;
    if (!image_reader_.reorder_hwc_to_chw(input, chw) || chw.pixels.size() < static_cast<size_t>(total_pixels) * 3) {
        image.release();
        return result;
    }

    const uint8_t* src = chw.pixels.data();
    const float scale = 1.0f / 255.0f;

    for (int c = 0; c < 3; ++c) {
        const size_t channel_offset = static_cast<size_t>(c) * total_pixels;
        for (int i = 0; i < total_pixels; ++i) {
            float normalized = (static_cast<float>(src[channel_offset + i]) * scale - 0.5f) * 2.0f;
            result[channel_offset + i] = bf16(normalized);
        }
    }

    image_reader_.recycle(chw);
    image.release();
    return result;
}
