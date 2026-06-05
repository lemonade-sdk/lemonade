/// \file dequant.hpp
/// \brief dequant class
/// \author FastFlowLM Team
/// \date 2025-06-24
/// \version 0.9.24
/// \note This is a header file for the dequant class
#pragma once
#include "lm_config.hpp"
#include "npu_utils/npu_utils.hpp"

/// \brief dequant class
/// \note This is a class for the dequant layer
class GemmaTextDequant{
public:
    GemmaTextDequant(){}

    /// \brief Constructor
    /// \param config the configuration
    /// \param xclbin_name the xclbin name
    /// \param npu the npu manager
    GemmaTextDequant(LM_Config& config);
    ~GemmaTextDequant();

    /// \brief Generate the sequence
    /// \param seq the sequence
    /// \param D_in the input dimension
    /// \param D_out the output dimension
    /// \param weight_offset the weight offset
    void generate_seq(npu_sequence* seq, const uint32_t D_in, const uint32_t D_out, const uint32_t weight_offset);

    /// \brief get q_group_id
    /// \return the q group id
    int q_id();
    /// \brief get qw_group_id
    /// \return the qw group id
    int qw_id();

private:
    struct Impl;
    Impl* _impl;

};
