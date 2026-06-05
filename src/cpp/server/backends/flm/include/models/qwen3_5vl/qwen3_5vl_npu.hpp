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

// #define QWEN3_5_VL_4B 1

// #ifdef QWEN3_5_VL_4B


    // constexpr unsigned int QWEN3_5_PATCH_SIZE = 16;
    // constexpr unsigned int QWEN3_5_IMAGE_MERGE_SIZE=2;
    // constexpr unsigned int QWEN3_5_SPATIAL_MERGE_SIZE=2;
    // constexpr unsigned int QWEN3_5_SHORTEST_EDGE = 65536;
    // constexpr unsigned int QWEN3_5_LONGEST_EDGE = 16777216;
    // constexpr float QWEN3_5_VISION_RESCALE_FACTOR = 0.00392156862745098;
    // constexpr float QWEN3_5_VISION_RESCALE_IMAGE_MEAN = 0.5f;
    // constexpr float QWEN3_5_VISION_RESCALE_IMAGE_STD = 0.5f;
    // constexpr unsigned int QWEN3_5_TEMPORAL_PATCH_SIZE = 2;
    // constexpr unsigned int QWEN3_5_MERGE_SIZE = QWEN3_5_IMAGE_MERGE_SIZE;




// #endif

typedef struct {
    int height;
    int width;
    int height_resized;  // assigned by image preprocessing
    int width_resized;
    int grid_h;
    int grid_w;

    bytes _data;

} qwen3_5vl_image_t;



typedef struct {
    std::vector<qwen3_5vl_image_t> images;
    std::vector<bf16> _data__processed;
    unsigned int num_images;
}qwen3_5vl_image_payload_t;



class qwen3_5vl_npu : public causal_lm{
public:
    /// \brief  initialize the qwen3vl_npu
    /// \param config the configuration
    /// \param npu_instance the npu instance
    qwen3_5vl_npu(LM_Config config, npu_xclbin_manager *npu_instance, int MAX_L = 4096);
    ~qwen3_5vl_npu();

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

    // parameters for vision process in qwen3.5 vl

    unsigned int QWEN3_5_PATCH_SIZE;
    unsigned int QWEN3_5_IMAGE_MERGE_SIZE;
    unsigned int QWEN3_5_SPATIAL_MERGE_SIZE;
    unsigned int QWEN3_5_SHORTEST_EDGE;
    unsigned int QWEN3_5_LONGEST_EDGE;
    float QWEN3_5_VISION_RESCALE_FACTOR;
    float QWEN3_5_VISION_RESCALE_IMAGE_MEAN;
    float QWEN3_5_VISION_RESCALE_IMAGE_STD;
    unsigned int QWEN3_5_TEMPORAL_PATCH_SIZE;
    unsigned int QWEN3_5_MERGE_SIZE;


    inline void load_vision_preprocess_parameters(LM_Config& config){
        // Note: this should be called by Impl:: constructor
        QWEN3_5_PATCH_SIZE  = config._vision_config.value("QWEN3_5_PATCH_SIZE", -1);
        QWEN3_5_IMAGE_MERGE_SIZE = config._vision_config.value("QWEN3_5_IMAGE_MERGE_SIZE", -1);
        QWEN3_5_SPATIAL_MERGE_SIZE = config._vision_config.value("QWEN3_5_SPATIAL_MERGE_SIZE", -1);
        QWEN3_5_SHORTEST_EDGE = config._vision_config.value("QWEN3_5_SHORTEST_EDGE", -1);
        QWEN3_5_LONGEST_EDGE = config._vision_config.value("QWEN3_5_LONGEST_EDGE", -1);
        QWEN3_5_VISION_RESCALE_FACTOR = config._vision_config.value("QWEN3_5_VISION_RESCALE_FACTOR", -1.0f);
        QWEN3_5_VISION_RESCALE_IMAGE_MEAN = config._vision_config.value("QWEN3_5_VISION_RESCALE_IMAGE_MEAN", -1.0f);
        QWEN3_5_VISION_RESCALE_IMAGE_STD = config._vision_config.value("QWEN3_5_VISION_RESCALE_IMAGE_STD", -1.0f);
        QWEN3_5_TEMPORAL_PATCH_SIZE = config._vision_config.value("QWEN3_5_TEMPORAL_PATCH_SIZE", -1);

        QWEN3_5_MERGE_SIZE = QWEN3_5_IMAGE_MERGE_SIZE;

    }
private:
    struct Impl;
    Impl* _impl;
};
