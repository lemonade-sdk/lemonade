/// \file auto_embedding_Mmdel.cpp
/// \brief AutoEmbeddingModel class
/// \author FastFlowLM Team
/// \date 2025-10-23
/// \version 0.9.24
/// \note This is a source file for the AutoEmbeddingModel class

#include "AutoEmbeddingModel/modeling_gemma_embedding.hpp"

std::unordered_set<std::string> embeddingModelTags = {
        "embed-gemma", "embed-gemma:300m"
};

AutoEmbeddingModel::AutoEmbeddingModel(xrt::device* npu_device_inst, std::string current_model) {
    this->npu_device_inst = npu_device_inst;
    this->current_model = current_model;
}


std::string AutoEmbeddingModel::get_current_model() {
    return this->current_model;
}

/// \brief Setup the tokenizer
/// \note The function will setup the tokenizer
nlohmann::json AutoEmbeddingModel::_shared_setup_tokenizer(std::string model_path) {
    // load tokenizer configurations
    #ifdef _WIN32
    std::string tokenizer_config_path = model_path + "\\tokenizer_config.json";
    #else
    std::string tokenizer_config_path = model_path + "/tokenizer_config.json";
    #endif
    std::ifstream fs_config(tokenizer_config_path, std::ios::in | std::ios::binary);
    if (fs_config.fail()) {
        std::cerr << "Cannot open " << tokenizer_config_path << std::endl;
        exit(1);
    }
    std::string data_config;
    fs_config.seekg(0, std::ios::end);
    size_t size_config = static_cast<size_t>(fs_config.tellg());
    fs_config.seekg(0, std::ios::beg);
    data_config.resize(size_config);
    fs_config.read(data_config.data(), size_config);
    fs_config.close();
    auto tokenizer_config = nlohmann::json::parse(data_config);

    return tokenizer_config;
}


void AutoEmbeddingModel::_shared_load_model(std::string model_path, json model_info, bool enable_preemption) {
    if (this->is_model_loaded && this->model_path == model_path) {
        header_print("FLM", "Model already loaded: " << this->model_path);
        return;
    }

    this->model_path = model_path;
    header_print("FLM", "Loading model: " << this->model_path);
    this->lm_config = std::make_unique<LM_Config>();
    this->lm_config->from_pretrained(this->model_path);

    if (this->npu_device_inst == nullptr) {
        header_print("ERROR", "NPU device instance is nullptr");
        exit(1);
    }
    this->npu = std::make_unique<npu_xclbin_manager>(npu_device::device_npu2, this->npu_device_inst, enable_preemption);
    this->enable_preemption = enable_preemption;

    this->is_model_loaded = true;
    this->tokenizer = std::make_unique<Tokenizer>(this->model_path);

    this->embedding_model_impl = std::make_unique<gemma_embedding>(*this->lm_config, this->npu.get());

    this->q4nx = std::make_unique<Q4NX>(this->model_path);

    this->embedding_model_impl->load_weights(*this->q4nx);

    this->q4nx.reset();
}

buffer<bf16> AutoEmbeddingModel::_shared_embed(std::vector<int>& tokens) {
    buffer<bf16> y = this->embedding_model_impl->embed(tokens);
    return y;
}
