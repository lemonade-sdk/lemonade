/// \file phi4_npu_sequence.hpp
/// \brief phi4_npu_sequence class
/// \author FastFlowLM Team
/// \date 2025-06-24
/// \version 0.9.10
/// \note This is a header file for the phi4_npu_sequence class
#pragma once
#include "npu_utils/npu_instr_utils.hpp"
#include "lm_config.hpp"

/// \brief phi4_npu_sequence class
/// \note This is a class for the phi4_npu_sequence
class phi4_npu_sequence{
public:
    phi4_npu_sequence(){}

    /// \brief Constructor
    /// \param config the configuration
    /// \param MAX_L the max length
    phi4_npu_sequence(LM_Config config, uint32_t MAX_L);
    ~phi4_npu_sequence();

    /// \brief Generate the rtp sequence
    /// \param seq the sequence
    /// \param L the length
    void gen_rtp_seq(npu_sequence* seq, const uint32_t L);

    /// \brief Generate the layer sequence
    /// \param seq the sequence
    /// \param L the length
    void gen_layer_seq(npu_sequence* seq, const uint32_t L);

    /// \brief Generate the dequant sequence
    /// \param seq the sequence
    /// \param D_in the input dimension
    /// \param D_out the output dimension
    /// \param weight_offset the weight offset
    void gen_dequant_seq(npu_sequence* seq, const size_t D_in, const size_t D_out, const size_t weight_offset);

    /// \brief Generate the lm head sequence
    /// \param seq the sequence
    void gen_lm_head_seq(npu_sequence* seq);

    /// \brief Set the max length
    /// \param MAX_L the max length
    void set_max_length(const uint32_t MAX_L);

    /// \brief Generate the mha engine sequence
    /// \param seq the sequence
    /// \param L_begin the begin length
    /// \param L_end the end length
    void gen_mha_engine_seq(npu_sequence* seq, const uint32_t L_begin, const uint32_t L_end);

    /// \brief Get the k03 offset
    size_t get_k03_offset() const;
    size_t get_k47_offset() const;
    size_t get_v03_offset() const;
    size_t get_v47_offset() const;

private:
    struct Impl;
    Impl* _impl;
};
