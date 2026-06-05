/// \file modeling_Qwen3VL_image.cpp
/// \brief Gemma4e image processing implementation
/// \author FastFlowLM Team
/// \date 2025-09-01
/// \version 0.9.24
/// \note This is a source file for the Gemma4e image processing functionality

#include "AutoModel/modeling_gemma4e.hpp"
#include <utility>
gemma4e_image_t Gemma4e::load_image(const std::string &filename)
{
    gemma4e_image_t empty_result;
    image_data_t decoded;
    image_data_t reordered;
    if (!image_reader_.load_image(filename, decoded))
    {
        return empty_result;
    }

    if (!image_reader_.reorder_hwc_to_chw(decoded, reordered))
    {
        image_reader_.recycle(decoded);
        return empty_result;
    }

    image_reader_.recycle(decoded);

    gemma4e_image_t result;
    result.width = reordered.width;
    result.height = reordered.height;
    result._data = std::move(reordered.pixels);
    image_reader_.recycle(reordered);
    return result;
}

gemma4e_image_t Gemma4e::load_image_base64(const std::string &base64_string)
{
    gemma4e_image_t empty_result;
    image_data_t decoded;
    image_data_t reordered;
    if (!image_reader_.load_image_base64(base64_string, decoded))
    {
        return empty_result;
    }

    if (!image_reader_.reorder_hwc_to_chw(decoded, reordered))
    {
        image_reader_.recycle(decoded);
        return empty_result;
    }

    image_reader_.recycle(decoded);

    gemma4e_image_t result;
    result.width = reordered.width;
    result.height = reordered.height;
    result._data = std::move(reordered.pixels);
    image_reader_.recycle(reordered);
    return result;
}






std::vector<uint8_t> Gemma4e::aspect_ratio_preserving_resize(
    gemma4e_image_t &image,
    int patch_size,
    int max_patches,
    int pooling_kernel_size)
{

    int height = image.height;
    int width = image.width;
    int channels =3 ;



    int target_height = height;
    int target_width = width;
    {
        // the get_aspect_ratio_preserving_size() in python
        int total_px = height * width;
        int target_px = max_patches * (patch_size * patch_size);
        double factor = std::sqrt(static_cast<double>(target_px) / total_px);
        double ideal_height = factor * height;
        double ideal_width = factor * width;
        int side_mult = pooling_kernel_size * patch_size;

        target_height = static_cast<int>(std::floor(ideal_height / side_mult)) * side_mult;
        target_width = static_cast<int>(std::floor(ideal_width / side_mult)) * side_mult;

        if (target_height == 0 && target_width == 0) {
            std::cerr << "Attempting to resize to a 0 x 0 image."<< std::endl;
            exit(-1);
        }

        int max_side_length = (max_patches / (pooling_kernel_size * pooling_kernel_size)) * side_mult;

        if (target_height == 0) {
            target_height = side_mult;
            target_width = std::min(
                static_cast<int>(std::floor(static_cast<double>(width) / height)) * side_mult,
                max_side_length
            );
        } else if (target_width == 0) {
            target_width = side_mult;
            target_height = std::min(
                static_cast<int>(std::floor(static_cast<double>(height) / width)) * side_mult,
                max_side_length
            );
        }

        if (target_height * target_width > target_px) {

            std::cerr << "Resized image exceeds max_patches"<< std::endl;
            exit(-1);
        }
    }

    if(target_height == height && target_width == width){
        image.width_resized = target_width;
        image.height_resized = target_height;

        std::vector<uint8_t> result(channels * height * width);
        memcpy(result.data(), image._data.data(), channels * height * width);

        image._data.free();
        return result;


    }else{
        // This is the point where we will need to do the

        // Trigger the resize, which is the cubic
        auto resized_image = imgproc::resize_bicubic_antialias_rgb_planar_optimized(

            image._data.data(),
            width, height,
            target_width, target_height,
            true
        );


        image.width_resized = target_width;
        image.height_resized = target_height;
        image._data.free();
        return resized_image;

    }








}

///@brief: preprocess the image for Gemma4e model
///@note: Converts uint8 image to BF16 format, data is already in (3, H, W) CHW layout
///@param: image: the image to preprocess (already in CHW format)
///@return: the preprocessed image in BF16 format
void Gemma4e::preprocess_image(
    gemma4e_image_t &image,
    std::pair<int, int> & patch_element_per_patch,
    uint32_t & valid_patch_size, // the unpadded size per image
    std::vector<bf16> &pixel_values,
    std::vector<int> &image_grid_pairs, // [num_of_position_id][x, y]
    uint32_t &num_soft_tokens

)
{

    //std::cout << "hit preprocess_image, image size: " << image.width << "x" << image.height << ", pixel count: " << (image.width * image.height) << std::endl;
    gemma4e_npu *lm_engine_gemma4e_ptr = reinterpret_cast<gemma4e_npu *>(this->lm_engine.get());
    int max_patches = this->image_softtoken_budget * lm_engine_gemma4e_ptr->GEMMA4E_POOLING_KERNEL_SIZE * lm_engine_gemma4e_ptr->GEMMA4E_POOLING_KERNEL_SIZE;

    // first, do_resize
    std::vector<uint8_t> resized_image_data = aspect_ratio_preserving_resize(
        image,
        lm_engine_gemma4e_ptr->GEMMA4E_VISION_PATCH_SIZE,
        max_patches,
        lm_engine_gemma4e_ptr->GEMMA4E_POOLING_KERNEL_SIZE
    );

    // step 2, rescale and normaliuze
    std::vector<float> rescaled_and_normalized_bufer(
        image.width_resized * image.height_resized * 3
    );


    imgproc::rescale_and_normalize_optimized(
        resized_image_data.data(),
        rescaled_and_normalized_bufer.data(),
        image.width_resized, image.height_resized, 3,
        true, lm_engine_gemma4e_ptr->GEMMA4E_VISION_RESCALE_FACTOR,
        false, lm_engine_gemma4e_ptr->GEMMA4E_VISION_IMAGE_MEAN, lm_engine_gemma4e_ptr->GEMMA4E_VISION_IMAGE_STD
    );

    auto patch_height = image.height_resized / lm_engine_gemma4e_ptr->GEMMA4E_VISION_PATCH_SIZE;
    auto patch_width = image.width_resized / lm_engine_gemma4e_ptr->GEMMA4E_VISION_PATCH_SIZE;
    int num_patches = patch_height * patch_width;
    int patch_size = lm_engine_gemma4e_ptr->GEMMA4E_VISION_PATCH_SIZE;
    int num_channels = 3;
    int elements_per_patch = patch_size * patch_size * num_channels;

    std::vector<float> patches(num_patches * elements_per_patch);

    {
        // the convert_image_to_parchtes operation in python

        for (int ph = 0; ph < patch_height; ++ph) {
            for (int pw = 0; pw < patch_width; ++pw) {
                int patch_idx = ph * patch_width + pw;
                for (int c = 0; c < num_channels; ++c) {
                    for (int y = 0; y < patch_size; ++y) {
                        for (int x = 0; x < patch_size; ++x) {
                            int src_y = ph * patch_size + y;
                            int src_x = pw * patch_size + x;
                            int src_idx = (c * image.height_resized + src_y) * image.width_resized + src_x;

                            int dst_idx = ((((ph * patch_width) + pw) * patch_size + y) * patch_size + x) * num_channels + c;
                            patches[dst_idx] = rescaled_and_normalized_bufer[src_idx];
                        }
                    }
                }
            }
        }



        num_soft_tokens =  num_patches / (  lm_engine_gemma4e_ptr->GEMMA4E_POOLING_KERNEL_SIZE * lm_engine_gemma4e_ptr->GEMMA4E_POOLING_KERNEL_SIZE);


    }


    // now. step 5

    image_grid_pairs.resize(num_patches * 2);
    for (int ph = 0; ph < patch_height; ++ph) {
        for (int pw = 0; pw < patch_width; ++pw) {
            int patch_idx = ph * patch_width + pw;
            image_grid_pairs[patch_idx * 2 + 0] = pw; // x
            image_grid_pairs[patch_idx * 2 + 1] = ph; // y
        }
    }

    valid_patch_size = num_patches;
    // Step 6. Pad patches and positions to `max_patches`
    int padding_length = max_patches - num_patches;
    if (padding_length > 0) {
        patches.resize(max_patches * elements_per_patch, 0.0f);
        image_grid_pairs.resize(max_patches * 2, -1);
    }



    // Step 7. Convert patches to BF16 and store in pixel_values
    pixel_values.resize(patches.size());
    for (size_t i = 0; i < patches.size(); ++i) {
        pixel_values[i] = static_cast<bf16>(patches[i]);
    }


    // store the logical 2D shape of pixel_values: (max_patches, elements_per_patch)
    // std::cout << "padding of " << padding_length << " patches is applied." << std::endl;
    // std::cout << "image_grid_pairs size: " << image_grid_pairs.size() << ", expected: " << max_patches * 2 << std::endl;
    patch_element_per_patch.first = max_patches;
    patch_element_per_patch.second = elements_per_patch;





}
