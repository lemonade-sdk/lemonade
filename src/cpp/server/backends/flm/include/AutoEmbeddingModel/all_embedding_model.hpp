/// \file all_embedding_models.hpp
/// \brief get_auto_embedding_model func
/// \author FastFlowLM Team
/// \date 2025-10-23
/// \version 0.9.24
/// \note This is a header file for get_auto_embedding_model func
#pragma once

#ifndef FASTFLOWLM_LINUX_LIMITED_MODELS
#include "modeling_gemma_embedding.hpp"
#endif

inline std::string complete_simple_embedding_tag(std::string model_tag) {
    if (model_tag == "embed-gemma:300m")
        return "embed-gemma:300m";
    else
        return model_tag;
}


inline std::pair<std::string, std::unique_ptr<AutoEmbeddingModel>> get_auto_embedding_model(const std::string& model_tag, xrt::device* npu_device_inst) {

#ifndef FASTFLOWLM_LINUX_LIMITED_MODELS
    static std::unordered_set<std::string> gemma_embed_tags = {
        "embed-gemma:300m"
    };


    std::unique_ptr<AutoEmbeddingModel> auto_embedding_engine = nullptr;
    std::string new_model_tag = complete_simple_embedding_tag(model_tag);
    if (gemma_embed_tags.count(model_tag)) // tag
        auto_embedding_engine = std::make_unique<Gemma_Embedding>(npu_device_inst);
    else {
        new_model_tag = "embed-gemma:300m"; // No arguments, use default tag
        auto_embedding_engine = std::make_unique<Gemma_Embedding>(npu_device_inst);
    }

    return std::make_pair(new_model_tag, std::move(auto_embedding_engine));
#else
    // Embedding models not supported on Linux limited builds
    throw std::runtime_error("Embedding models are not supported in this build");
#endif
}
