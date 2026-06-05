/// \file gemm.hpp
/// \brief gemm class
/// \author FastFlowLM Team
/// \date 2025-06-24
/// \version 0.9.10
/// \note This is a header file for the gemm class
#pragma once
#include "lm_config.hpp"
#include "npu_utils/npu_instr_utils.hpp"

/// \brief gemm class
/// \note This is a class for the gemm layer
class Gemm{
public:

    typedef enum: int {
        NO_Activation = 0,
        GeLU          = 1,
        SiLU          = 2
    } Activation_Type_t;
    Gemm(){}

    /// \brief Constructor
    /// \param config the configuration
    /// \param xclbin_name the xclbin name
    /// \param npu the npu manager
    Gemm(LM_Config& config);
    ~Gemm();

    /// \brief Generate the sequence
    /// \param seq the npu sequence
    /// \param M the M dimension
    /// \param K the K dimension
    /// \param N the N dimension
    /// \param weight_offset the weight offset
    /// \param ADD_BIAS whether to add bias
    /// \param OUTPUT_MODE the output activation mode
    /// \param bias_offset the bias offset
    void generate_seq(npu_sequence* seq, const uint32_t M, const uint32_t K, const uint32_t N, const uint32_t weight_offset, bool ADD_BIAS, Activation_Type_t OUTPUT_MODE, const uint32_t bias_offset);
    void generate_seq(npu_sequence* seq, const uint32_t M, const uint32_t K, const uint32_t N, const uint32_t weight_offset, bool ADD_BIAS, Activation_Type_t OUTPUT_MODE, const uint32_t bias_offset,
        const uint32_t output_offset
    );
    uint32_t get_m() const;
    uint32_t get_k() const;
    uint32_t get_n() const;

private:
    struct Impl;
    Impl* _impl;

};
