/// \file qwen2vl_npu.hpp
/// \brief qwen2vl_npu class
/// \author FastFlowLM Team
/// \date 2026-01-23
/// \version 0.9.28
/// \note This is a header file for the qwen2vl_npu class
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

constexpr unsigned int QWEN2_PATCH_SIZE = 14;
constexpr unsigned int QWEN2_IMAGE_MERGE_SIZE=2;
constexpr unsigned int QWEN2_SPATIAL_MERGE_SIZE=2;
constexpr unsigned int QWEN2_SHORTEST_EDGE = 3136;
constexpr unsigned int QWEN2_LONGEST_EDGE = 12845056;
constexpr float QWEN2_VISION_RESCALE_FACTOR = 0.00392156862745098;
constexpr float QWEN2_VISION_RESCALE_IMAGE_MEAN_R = 0.48145466f;
constexpr float QWEN2_VISION_RESCALE_IMAGE_MEAN_G =  0.4578275f;
constexpr float QWEN2_VISION_RESCALE_IMAGE_MEAN_B = 0.40821073f;
constexpr float QWEN2_VISION_RESCALE_IMAGE_STD_R = 0.26862954f;
constexpr float QWEN2_VISION_RESCALE_IMAGE_STD_G = 0.26130258f;
constexpr float QWEN2_VISION_RESCALE_IMAGE_STD_B = 0.27577711f;
constexpr unsigned int QWEN2_WINDOW_ATTENTION_PIXEL_SIZE = 122;
constexpr unsigned int QWEN2_TEMPORAL_PATCH_SIZE = 2;
constexpr unsigned int QWEN2_MERGE_SIZE = 2;
typedef struct {
    int height;
    int width;
    int height_resized;  // assigned by image preprocessing
    int width_resized;
    int grid_h;
    int grid_w;

    bytes _data;

} qwen2vl_image_t;



typedef struct {
    std::vector<qwen2vl_image_t> images;
    std::vector<bf16> _data__processed;
    unsigned int num_images;
}qwen2vl_image_payload_t;


class qwen2vl_npu : public causal_lm{
public:
    /// \brief  initialize the qwen2vl_npu
    /// \param config the configuration
    /// \param npu_instance the npu instance
    qwen2vl_npu(LM_Config config, npu_xclbin_manager *npu_instance, int MAX_L = 4096);
    ~qwen2vl_npu();

    /// \brief forward the qwen2vl_npu
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
private:
    struct Impl;
    Impl* _impl;
};
