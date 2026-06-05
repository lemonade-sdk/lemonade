/// \file safe_tensors.hpp
/// \brief SafeTensors class
/// \author FastFlowLM Team
/// \date 2025-06-24
/// \version 0.9.24
/// \note This class is used to load weights from a safe-tensors file.
#pragma once

#include "typedef.hpp"
#include "buffer.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
#include <fstream>

/// \brief Tensor metadata
typedef struct {
    std::string name;
    std::vector<size_t> shape;
    std::string dtype;
    std::vector<size_t> offsets;
    size_t size;
    size_t byte_size;
} tensor_metadata;

/// \brief SafeTensors class
class SafeTensors{
private:
    std::string model_path;
    std::ifstream file;
    nlohmann::json metadata;
    size_t _get_data_size(tensor_metadata tensor_meta);
    void _load_tensors();
    void _open_file();

protected:
    std::vector<tensor_metadata> tensors_data;

public:
    /// \brief Constructor
    /// \param model_path the model path
    SafeTensors(){
        this->model_path = "";
    }

    /// \brief Constructor
    /// \param model_path the model path
    SafeTensors(const std::string& model_path);

    /// \brief Destructor
    ~SafeTensors();

    /// \brief Load the weights
    /// \param weight_buffer the weight buffer
    /// \param weights_name the weights name
    /// \return the weights name
    std::string load_weights(bytes& weight_buffer, std::string weights_name);

    /// \brief Get the tensor metadata
    /// \param tensor_name the tensor name
    /// \return the tensor metadata
    tensor_metadata get_tensor_metadata(std::string tensor_name);

    /// \brief Write the safetensors
    /// \param output_path the output path
    void write_safetensors(std::string output_path);

    /// \brief Get the metadata
    /// \return the metadata
    nlohmann::json get_metadata();

    /// \brief Switch the model
    /// \param new_model_path the new model path
    void switch_model(std::string new_model_path);
};
