/// \file q4_npu_eXpress.hpp
/// \brief Q4NX class
/// \author FastFlowLM Team
/// \date 2025-06-24
/// \version 0.9.24
/// \note This class is used to quantize the model.
#pragma once

#include "safe_tensors.hpp"

/// \brief Q4NX class
/// \note This class is for Q4NX reordering and model conversion.
class Q4NX : public SafeTensors {
private:
    bool is_a_quantized_model;
    int layers;
    nlohmann::json _process_json_header(std::vector<tensor_metadata>& new_tensors_data);
    void _convert_llama(std::ofstream& output_file);
    void _grap_metadata();
    void _q4nx_reorder(bytes& q4nx_weight, buffer<u32>& q, buffer<bf16>& scale, buffer<i32>& zero_point, int columns);
    void _to_bf16(bytes& weight);
    tensor_metadata _convert_to_q4nx(tensor_metadata tensor_meta, bool is_quantized, size_t& offset);
public:
    struct npu_quantize_block;
    typedef npu_quantize_block q4nx;

    /// \brief Constructor
    /// \param model_path the model path
    Q4NX(std::string model_path);

    /// \brief Convert the model
    /// \param output_path the output path
    void convert_model(std::string output_path);

    /// \brief Dequantize the weight
    /// \param weight the weight
    /// \param q4nx_weight the q4nx weight
    /// \param columns the columns
    template<typename T>
    static void q4nx_dequantize(bytes& weight, bytes& q4nx_weight, const int columns);

    /// \brief Dequantize the weight
    /// \param weight the weight
    /// \param q4nx_weight the q4nx weight
    /// \param columns the columns
    template<typename T>
    static void q4nx_dequantize(buffer<T>& weight, buffer<u32>& q, buffer<bf16>& scale, buffer<i32>& zero_point, const int columns);

    /// \brief Get the weight per chunk
    /// \return the weight per chunk
    size_t get_weight_per_chunk() const;

    /// \brief Get the block size
    /// \return the block size
    size_t get_block_size() const;

    /// \brief Dequantize the weight for mm
    /// \param weight the weight
    static void q4nx_dequantize_for_mm(buffer<bf16>& weight, bytes& q4nx_weight, const int columns);
};
