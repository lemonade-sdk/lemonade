/// \file whisper_npu.hpp
/// \brief whisper_npu class
/// \author FastFlowLM Team
/// \date 2025-10-17
/// \version 0.9.24
/// \note This is a header file for the whisper_npu class
#pragma once
#include "lm_config.hpp"
#include "npu_utils/npu_utils.hpp"
#include "tensor_utils/q4_npu_eXpress.hpp"
#include "modules/embedding.hpp"
#include "modules/lm_head.hpp"
#include "modules/dequant.hpp"
#include "tensor_2d.hpp"
#include "utils/utils.hpp"
#if USEAVX2
#include <immintrin.h>  // For AVX intrinsics
#endif


class whisper_npu{
public:
    /// \brief  initialize the whisper_npu
    /// \param config the configuration
    /// \param npu_instance the npu instance
    whisper_npu(Whisper_Config config, npu_xclbin_manager *npu_instance, int MAX_L = 448);
    ~whisper_npu();

    /// \brief forward the whisper_npu
    /// \param ids the ids
    /// \return the output tensor
    buffer<bf16> encode_audio(buffer<bf16>& mel_feature);

    buffer<bf16> decode_audio(int last_ids);

    /// \brief load the weights
    /// \param q4nx the q4nx
    void load_weights(Q4NX& q4nx);

    /// \brief get the current context length
    /// \return the current context length
    int get_current_context_length();

    void clear_context();

private:
    struct Impl;
    Impl* _impl;
};
