/// \file qwen3vl_npu.hpp
/// \brief qwen3vl_npu class
/// \author FastFlowLM Team
/// \date 2026-01-23
/// \version 0.9.28
/// \note This is a header file for the qwen3vl_npu class
#pragma once
#include "lm_config.hpp"
#include "npu_utils/npu_utils.hpp"
#include "tensor_utils/q4_npu_eXpress.hpp"
#include "modules/embedding.hpp"
#include "modules/lm_head.hpp"
#include "modules/gemm.hpp"
#include "modules/dequant.hpp"
#include "tensor_2d.hpp"
#include "utils/utils.hpp"
#include "causal_lm.hpp"
#if USEAVX2
#include <immintrin.h>  // For AVX intrinsics
#endif

// some helper functions for convenience
constexpr int GEMMA4E_IS_GLOBAL_MASK = 0x00000001;
constexpr int GEMMA4E_IS_SKIP_MASK = 0x00000002;

typedef enum :int {
    e_gemma4e_swa_layer = 0,
    e_gemma4e_global_layer = GEMMA4E_IS_GLOBAL_MASK,
    e_gemma4e_swa_layer_skip = GEMMA4E_IS_SKIP_MASK,
    e_gemma4e_global_layer_skip = GEMMA4E_IS_GLOBAL_MASK | GEMMA4E_IS_SKIP_MASK,
    e_gemma4e_total_layer_types = 4
} gemma4e_layer_type_t;

inline bool is_swa_layer(gemma4e_layer_type_t layer) {
    return (layer & GEMMA4E_IS_GLOBAL_MASK) == 0;
}

inline bool is_global_layer(gemma4e_layer_type_t layer) {
    return (layer & GEMMA4E_IS_GLOBAL_MASK) != 0;
}

inline bool is_skip_layer(gemma4e_layer_type_t layer) {
    return (layer & GEMMA4E_IS_SKIP_MASK) != 0;
}

typedef struct {
    int height;
    int width;
    int height_resized;  // assigned by image preprocessing
    int width_resized;
    // int grid_h;
    // int grid_w;

    bytes _data;

} gemma4e_image_t;





struct gemma4e_image_payload_t{
    // original raw data
    std::vector<std::pair<int, int>> image_patch__element_per_patch; // [num_of_image][width, height]
    std::vector<uint32_t> valid_patch_size_per_image; // [num_of_image], the unpadded size per image
    std::vector<std::vector<bf16>> pixel_values; // [num_of_image][image_size], where image_size = height_resized * width_resized * 3
    std::vector< std::vector<int>> image_grid_pairs_per_image; // [num_of_image][num_of_position_id][x, y]
    std::vector<unsigned int> num_soft_tokens_per_image; // [num_of_image]
    unsigned int num_images;
};


struct gemma4e_audio_payload_t{
    // per-audio mel spectrogram data
    std::vector<std::vector<bf16>> mel_spectrograms;               // [num_audios][frames * bins], row-major
    std::vector<int> mel_spectrogram_frames_per_audio;             // [num_audios]
    std::vector<int> mel_spectrogram_bins_per_audio;               // [num_audios]
    unsigned int num_audios = 0;
    std::vector<unsigned int> num_soft_tokens_per_audio; // [num_audios]

};


typedef struct {
    gemma4e_image_payload_t image_payload;
    gemma4e_audio_payload_t audio_payload;
} gemma4e_multi_modal_payload_t;

class gemma4e_npu : public causal_lm{
public:
    /// \brief  initialize the qwen3vl_npu
    /// \param config the configuration
    /// \param npu_instance the npu instance
    gemma4e_npu(LM_Config config, npu_xclbin_manager *npu_instance, int MAX_L = 4096);
    ~gemma4e_npu();

    /// \brief forward the qwen3vl_npu
    /// \param ids the ids
    /// \return the output tensor
    buffer<bf16> forward(int ids) override;
    buffer<bf16> prefill(std::vector<int>& ids, void* payload = nullptr) override;

    /// \brief set the context length
    /// \param L the context length
    void set_context_length(int L) override;

    /// \brief load the weights
    /// \param q4nx the q4nx
    void load_weights(Q4NX& q4nx) override;

    /// \brief update the max length
    void clear_context() override;

    /// \brief get the k cache
    /// \param layer_idx the layer index
    /// \param idx the index
    /// \return the k cache
    buffer<bf16> get_k_cache(int layer_idx, int idx) override;

    /// \brief get the v cache
    /// \param layer_idx the layer index
    /// \param idx the index
    /// \return the v cache
    buffer<bf16> get_v_cache(int layer_idx, int idx) override;

    /// \brief update the max length
    /// \param MAX_L the max length
    void update_max_length(uint32_t MAX_L) override;

    /// \brief get the current context length
    /// \return the current context length
    int get_current_context_length() override;


    int checkpoint() override;
    int restore() override;

    // parameters for vision preprocessing in Gemma4e
    unsigned int GEMMA4E_VISION_MAX_POSITION_EMBEDDINGS;
    unsigned int GEMMA4E_VISION_NUM_HIDDEN_LAYERS;
    unsigned int GEMMA4E_VISION_NUM_ATTENTION_HEADS;
    unsigned int GEMMA4E_VISION_HIDDEN_SIZE;
    unsigned int GEMMA4E_VISION_INTERMEDIATE_SIZE;
    unsigned int GEMMA4E_VISION_HEAD_DIM;
    unsigned int GEMMA4E_VISION_PATCH_SIZE;
    float GEMMA4E_ROPE_THETA;
    unsigned int GEMMA4E_POOLING_KERNEL_SIZE;
    unsigned int GEMMA4E_POSITION_EMBEDDING_SIZE;
    unsigned int GEMMA4E_VISION_IMAGE_OUTPUT_SIZE;
    float GEMMA4E_VISION_RESCALE_FACTOR;
    float GEMMA4E_VISION_IMAGE_MEAN;
    float GEMMA4E_VISION_IMAGE_STD;

    // parameters for audio preprocessing in Gemma4e
    unsigned int Audio_MM_TILE_M;
    unsigned int Audio_MM_TILE_K;
    unsigned int Audio_MM_TILE_N;
    int Gemma4E_Audio_resample_rate;
    float Gemma4E_Audio_gradient_clipping;
    unsigned int Gemma4E_Audio_Multimodal_Output_SIZE;
    unsigned int Gemma4E_Audio_language_projection_output_size;
    unsigned int Gemma4E_Audio_HIDDEN_SIZE;
    unsigned int Gemma4E_Audio_INTERMEDIATE_SIZE;
    unsigned int Gemma4E_Audio_attention_chunk_size;
    unsigned int Gemma4E_Audio_attention_context_left;
    unsigned int Gemma4E_Audio_attention_context_right;
    unsigned int Gemma4E_Audio_num_attention_heads;
    unsigned int Gemma4E_Audio_num_attention_layers;
    unsigned int Gemma4E_Audio_conv1d_kernel_size;
    unsigned int Gemma4E_Audio_conv1d_stride;
    unsigned int Gemma4E_Audio_conv2d_kernel_size;
    unsigned int Gemma4E_Audio_conv2d_Stride;
    unsigned int Gemma4e_Audio_conv2d_Padding;
    unsigned int Gemma4E_Audio_subsampling_conv_channels_0;
    unsigned int Gemma4E_Audio_subsampling_conv_channels_1;
    float Gemma4E_Audio_attention_softcap;


    inline void load_vision_preprocess_parameters(LM_Config& config){
        // Note: this should be called by Impl:: constructor
        GEMMA4E_VISION_MAX_POSITION_EMBEDDINGS = config._vision_config.value("GEMMA4E_VISION_MAX_POSITION_EMBEDDINGS", -1);
        GEMMA4E_VISION_NUM_HIDDEN_LAYERS   = config._vision_config.value("GEMMA4E_VISION_NUM_HIDDEN_LAYERS", -1);
        GEMMA4E_VISION_NUM_ATTENTION_HEADS = config._vision_config.value("GEMMA4E_VISION_NUM_ATTENTION_HEADS", -1);
        GEMMA4E_VISION_HIDDEN_SIZE         = config._vision_config.value("GEMMA4E_VISION_HIDDEN_SIZE", -1);
        GEMMA4E_VISION_INTERMEDIATE_SIZE   = config._vision_config.value("GEMMA4E_VISION_INTERMEDIATE_SIZE", -1);
        GEMMA4E_VISION_HEAD_DIM            = config._vision_config.value("GEMMA4E_VISION_HEAD_DIM", -1);
        GEMMA4E_VISION_PATCH_SIZE          = config._vision_config.value("GEMMA4E_VISION_PATCH_SIZE", -1);
        GEMMA4E_ROPE_THETA                 = config._vision_config.value("GEMMA4E_ROPE_THETA", -1.0f);
        GEMMA4E_POOLING_KERNEL_SIZE        = config._vision_config.value("GEMMA4E_POOLING_KERNEL_SIZE", -1);
        GEMMA4E_POSITION_EMBEDDING_SIZE    = config._vision_config.value("GEMMA4E_POSITION_EMBEDDING_SIZE", -1);
        GEMMA4E_VISION_IMAGE_OUTPUT_SIZE   = config._vision_config.value("GEMMA4E_VISION_IMAGE_OUTPUT_SIZE", -1);
        GEMMA4E_VISION_RESCALE_FACTOR      = config._vision_config.value("GEMMA4E_VISION_RESCALE_FACTOR", -1.0f);
        GEMMA4E_VISION_IMAGE_MEAN          = config._vision_config.value("GEMMA4E_VISION_IMAGE_MEAN", -1.0f);
        GEMMA4E_VISION_IMAGE_STD           = config._vision_config.value("GEMMA4E_VISION_IMAGE_STD", -1.0f);
    }

    inline void load_audio_preprocess_parameters(LM_Config& config){
        Audio_MM_TILE_M = config._audio_config.value("Audio_MM_TILE_M", 128);
        Audio_MM_TILE_K = config._audio_config.value("Audio_MM_TILE_K", 512);
        Audio_MM_TILE_N = config._audio_config.value("Audio_MM_TILE_N", 64);
        Gemma4E_Audio_resample_rate = config._audio_config.value("Gemma4E_Audio_audio_resample_rate", -1);
        Gemma4E_Audio_gradient_clipping = config._audio_config.value("Gemma4E_Audio_gradient_clipping", -1.0f);
        Gemma4E_Audio_Multimodal_Output_SIZE = config._audio_config.value("Gemma4E_Audio_Multimodal_Output_SIZE", -1);
        Gemma4E_Audio_language_projection_output_size = config._audio_config.value("Gemma4E_Audio_language_projection_output_size", -1);
        Gemma4E_Audio_HIDDEN_SIZE = config._audio_config.value("Gemma4E_Audio_HIDDEN_SIZE", -1);
        Gemma4E_Audio_INTERMEDIATE_SIZE = config._audio_config.value("Gemma4E_Audio_INTERMEDIATE_SIZE", -1);
        Gemma4E_Audio_attention_chunk_size = config._audio_config.value("Gemma4E_Audio_attention_chunk_size", -1);
        Gemma4E_Audio_attention_context_left = config._audio_config.value("Gemma4E_Audio_attention_context_left", -1);
        Gemma4E_Audio_attention_context_right = config._audio_config.value("Gemma4E_Audio_attention_context_right", -1);
        Gemma4E_Audio_num_attention_heads = config._audio_config.value("Gemma4E_Audio_num_attention_heads", -1);
        Gemma4E_Audio_num_attention_layers = config._audio_config.value("Gemma4E_Audio_num_attention_layers", -1);
        Gemma4E_Audio_conv1d_kernel_size = config._audio_config.value("Gemma4E_Audio_conv1d_kernel_size", -1);
        Gemma4E_Audio_conv1d_stride = config._audio_config.value("Gemma4E_Audio_conv1d_stride", -1);
        Gemma4E_Audio_conv2d_kernel_size = config._audio_config.value("Gemma4E_conv2d_kernel_size", -1);
        Gemma4E_Audio_conv2d_Stride = config._audio_config.value("Gemma4E_conv2d_Stride", -1);
        Gemma4e_Audio_conv2d_Padding = config._audio_config.value("Gemma4e_conv2d_Padding", -1);
        Gemma4E_Audio_subsampling_conv_channels_0 = config._audio_config.value("Gemma4E_Audio_subsampling_conv_channels_0", -1);
        Gemma4E_Audio_subsampling_conv_channels_1 = config._audio_config.value("Gemma4E_Audio_subsampling_conv_channels_1", -1);
        Gemma4E_Audio_attention_softcap = config._audio_config.value("Gemma4E_Audio_attention_softcap", -1.0f);
    }

private:
    struct Impl;
    Impl* _impl;
};
