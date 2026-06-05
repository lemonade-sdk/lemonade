/// \file modeling_gemma_embedding.cpp
/// \brief Gemma_Embedding class
/// \author FastFlowLM Team
/// \date 2025-10-23
/// \version 0.9.24
/// \note This is a source file for the Gemma_Embedding class

#include "AutoEmbeddingModel/modeling_gemma_embedding.hpp"
#include "models/gemma_embedding/gemma_embedding.hpp"

Gemma_Embedding::Gemma_Embedding(xrt::device* npu_device_inst) : AutoEmbeddingModel(npu_device_inst, "embed-gemma:300m") {
}

Gemma_Embedding::~Gemma_Embedding() = default;

void Gemma_Embedding::load_model(std::string model_path, json model_info, bool enable_preemption) {
    this->_shared_load_model(model_path, model_info, enable_preemption);
}

std::vector<float> Gemma_Embedding::embed(std::string& text, embedding_task_type_t task_type) {
    std::string task_prefix = this->_get_task_prefix(task_type);
    std::string full_text = "<bos>" + task_prefix + text + "<eos>";
    std::vector<int> tokens = this->tokenizer->encode(full_text);
    if (tokens.size() > 2048) {
        // cut to 2047 and add <eos> (1)
        tokens.resize(2048);
        tokens[2047] = 1;
    }

    buffer<bf16> y = this->embedding_model_impl->embed(tokens);
    std::vector<float> result(y.size());
    for (int i = 0; i < y.size(); i++) {
        result[i] = static_cast<float>(y[i]);
    }
    return result;
}
