/// \file embedding.hpp
/// \brief embedding class
/// \author FastFlowLM Team
/// \date 2025-06-24
/// \version 0.9.10
/// \note This is a header file for the embedding class.

#pragma once

#include "typedef.hpp"
#include "tensor_utils/safe_tensors.hpp"

/// \brief embedding class
/// \note This is a class for the embedding layer
class Embedding{
public:
    vdtype w;
    vdtype y;
    int vocab_size;
    int d_model;
    int buffer_id;

    /// \brief Constructor
    /// \param vocab_size the vocabulary size
    /// \param d_model the model dimension
    Embedding(){};
    Embedding(int vocab_size, int d_model){
        this->vocab_size = vocab_size;
        this->d_model = d_model;
        this->w.resize((size_t)vocab_size * d_model);
        this->y.resize(d_model);
    }

    /// \brief Forward pass
    /// \param x the input tensor
    /// \return the output tensor
    vdtype forward(int x){
        this->y.copy_from(this->w.begin() + (size_t)x * this->d_model, this->d_model);
        return this->y;
    }

    /// \brief Forward pass
    /// \param x the input tensor
    /// \param y the output tensor
    /// \return the output tensor
    vdtype forward(int x, vdtype& y){
        y.copy_from(this->w.begin() + (size_t)x * this->d_model, this->d_model);
        return y;
    }

    /// \brief Initialize the weights
    /// \param safe_tensors the safe tensors
    /// \param weight_name the weight name, which is the name of the weight file
    void init_weights(SafeTensors* safe_tensors, std::string weight_name){
        safe_tensors->load_weights(this->w, weight_name + ".weight");
    }
};
