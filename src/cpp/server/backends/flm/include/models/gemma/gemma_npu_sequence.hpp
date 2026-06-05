/// \file gemma_npu_sequence.hpp
/// \brief gemma_npu_sequence class
/// \author FastFlowLM Team
/// \date 2026-01-07
/// \version 0.9.25
/// \note This is a header file for the gemma_npu_sequence class
#pragma once
#include "npu_utils/npu_instr_utils.hpp"
#include "lm_config.hpp"

/// \brief gemma_npu_sequence class
/// \note This is a class for the gemma_npu_sequence
class gemma_npu_sequence{
public:
    gemma_npu_sequence(){}

    /// \brief Constructor
    /// \param config the configuration
    /// \param MAX_L the max length
    gemma_npu_sequence(LM_Config config, uint32_t MAX_L);
    ~gemma_npu_sequence();

    /// \brief Generate the rtp sequence
    /// \param seq the sequence
    /// \param L the length
    void gen_rtp_seq(npu_sequence* seq, const uint32_t L, bool is_sliding_window);

    /// \brief Generate the layer sequence
    /// \param seq the sequence
    /// \param L the length
    void gen_layer_seq(npu_sequence* seq, const uint32_t L, bool is_sliding_window);

    /// \brief Set the max length
    /// \param MAX_L the max length
    void set_max_length(const uint32_t MAX_L);

    /// \brief Generate the mha engine sequence
    /// \param seq the sequence
    /// \param L_begin the begin length
    /// \param L_end the end length
    void gen_mha_engine_seq(npu_sequence* seq, const uint32_t L_begin, const uint32_t L_end, bool is_sliding_window, int buffer_length);

    /// \brief Generate the lm head sequence
    /// \param seq the sequence
    void gen_lm_head_seq(npu_sequence* seq);

    /// \brief Get the k03 offset
    size_t get_k01_offset() const;
    size_t get_k23_offset() const;
    size_t get_v01_offset() const;
    size_t get_v23_offset() const;

private:
    struct Impl;
    Impl* _impl;
};
