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
class causal_lm {
public:
    causal_lm(){}
    virtual ~causal_lm(){}

    /// \brief forward the causal_lm
    /// \param ids the ids
    /// \return the output
    virtual buffer<bf16> forward(int ids) = 0;

    /// \brief prefill the causal_lm
    /// \param ids the ids
    /// \return the output
    virtual buffer<bf16> prefill(std::vector<int>& ids, void* payload = nullptr) = 0;

    /// \brief set the context length
    /// \param L the context length
    virtual void set_context_length(int L) = 0;

    /// \brief load the weights
    /// \param q4nx the q4nx
    virtual void load_weights(Q4NX& q4nx) = 0;

    /// \brief update the max length
    /// \param MAX_L the max length
    virtual void update_max_length(uint32_t MAX_L) = 0;

    /// \brief clear the context
    virtual void clear_context() = 0;

    /// \brief get the k cache
    /// \param layer_idx the layer index
    /// \param idx the index
    /// \return the k cache
    virtual buffer<bf16> get_k_cache(int layer_idx, int idx) = 0;

    /// \brief get the v cache
    /// \param layer_idx the layer index
    /// \param idx the index
    /// \return the v cache
    virtual buffer<bf16> get_v_cache(int layer_idx, int idx) = 0;

    /// \brief get the current context length
    /// \return the current context length
    virtual int get_current_context_length() = 0;

    virtual int checkpoint() = 0;

    virtual int restore() = 0;
};
