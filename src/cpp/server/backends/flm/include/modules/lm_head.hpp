/// \file lm_head.hpp
/// \brief lm_head class
/// \author FastFlowLM Team
/// \date 2025-06-24
/// \version 0.9.24
/// \note This is a header file for the lm_head class
#pragma once
#include "lm_config.hpp"
#include "tensor_utils/q4_npu_eXpress.hpp"
#include "npu_utils/npu_utils.hpp"

/// \brief lm_head class
/// \note This is a class for the lm_head layer
class LMHead{
public:
    LMHead(){}

    /// \brief Constructor
    /// \param config the configuration
    /// \param xclbin_name the xclbin name
    /// \param npu the npu manager
    LMHead(LM_Config config, npu_xclbin_manager *npu);
    ~LMHead();

    /// \brief Load the weights
    /// \param q4nx the q4nx
    void load_weights(Q4NX& q4nx);

    /// \brief Execute the lm_head
    void execute();

    /// \brief Wait for the lm_head
    /// \return the output tensor
    buffer<bf16> wait();

    /// \brief Get the output tensor
    /// \return the output tensor
    buffer<bf16> x_exposed();

private:
    struct Impl;
    Impl* _impl;

};
