/// \file causal_lm.hpp
/// \brief causal_lm class
/// \author FastFlowLM Team
/// \date 2025-06-24
/// \version 0.9.10
/// \note This class is a virtual class for causal language models
/// \note All other models should inherit from this class so that they can be used in the same way.
#pragma once
#include "tensor_utils/q4_npu_eXpress.hpp"
#include "tensor_2d.hpp"
#include "utils/utils.hpp"
#include "buffer.hpp"



/// \brief causal_lm class
class embedding_model {
public:
    embedding_model(){}
    virtual ~embedding_model(){}

    /// \brief load the weights
    /// \param q4nx the q4nx
    virtual void load_weights(Q4NX& q4nx) = 0;
    /// \brief embed the embedding_model
    /// \param x the input
    /// \return the output
    virtual buffer<bf16> embed(std::vector<int>& tokens) = 0;
};
