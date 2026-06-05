/// \file gemma_embedding.hpp
/// \brief gemma_embedding class
/// \author FastFlowLM Team
/// \date 2025-10-23
/// \version 0.9.24
/// \note This is a header file for the gemma_embedding class
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
#include "embedding_model.hpp"
#if USEAVX2
#include <immintrin.h>  // For AVX intrinsics
#endif


class gemma_embedding : public embedding_model{
public:
    /// \brief  initialize the gemma_embedding
    /// \param config the configuration
    /// \param npu_instance the npu instance
    gemma_embedding(LM_Config config, npu_xclbin_manager *npu_instance);
    ~gemma_embedding();

    /// \brief load the weights
    /// \param q4nx the q4nx
    void load_weights(Q4NX& q4nx) override;

    /// \brief embed the gemma_embedding
    /// \param tokens the tokens
    /// \return the output tensor
    buffer<bf16> embed(std::vector<int>& tokens) override;

private:
    struct Impl;
    Impl* _impl;
};
