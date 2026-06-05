/// \file dequant.hpp
/// \brief dequant class
/// \author FastFlowLM Team
/// \date 2025-06-24
/// \version 0.9.10
/// \note This is a header file for the dequant class
#pragma once
#include "lm_config.hpp"
#include "npu_utils/npu_instr_utils.hpp"

/// \brief dequant class
/// \note This is a class for the dequant layer
class Dequant{
public:
    Dequant(){}

    /// \brief Constructor
    /// \param config the configuration
    /// \param xclbin_name the xclbin name
    /// \param npu the npu manager
    Dequant(LM_Config& config);
    ~Dequant();

    /// @brief generate the dequant sequence
    /// @param seq: the sequence
    /// @param D_in: input dimension of the projection weight
    /// @param D_out: output dimension of the projection weight
    /// @param weight_offset: the weight offset in byte
    /// @param mode: dequant output mode
    void generate_dequant_q4_1_seq(npu_sequence* seq, const uint32_t D_in, const uint32_t D_out, const uint32_t weight_offset, int mode);
    void generate_dequant_q80_packed_in_q4nx_seq(npu_sequence* seq, const uint32_t D_in, const uint32_t D_out, const uint32_t weight_offset, int mode);
private:
    struct Impl;
    Impl* _impl;

};
