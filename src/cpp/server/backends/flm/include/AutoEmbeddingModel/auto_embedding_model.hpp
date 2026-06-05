/// \file auto_embedding_model.hpp
/// \brief AutoEmbeddingModel class
/// \author FastFlowLM Team
/// \date 2025-10-23
/// \version 0.9.24
/// \note This is a header file for the AutoEmbeddingModel class
#pragma once

#include <ctime>
#include <iomanip>
#include <sstream>
#include <memory>
#include <vector>
#include <iostream>
#include <string>
#include <type_traits>
#include <any>
#include "typedef.hpp"
#include "embedding_model.hpp"
#include "lm_config.hpp"
#include "tokenizer/tokenizer.hpp"
#include "modules/sampler.hpp"
#include "utils/utils.hpp"
#include "utils/profiler.hpp"
#include "tensor_utils/q4_npu_eXpress.hpp"
#include "npu_utils/npu_utils.hpp"
#include "minja/chat-template.hpp"
#include <nlohmann/json.hpp>

using json = nlohmann::ordered_json;


typedef enum : u8 {
    task_query = 0,
    task_document = 1,
    task_bitextmining = 2,
    task_clustering = 3,
    task_classification = 4,
    task_code_retrieval = 5,
    task_multilabel_classification = 6,
    task_sentence_similarity = 7,
    task_search_result = 8,
    task_summarization = 9,
} embedding_task_type_t;

extern std::unordered_set<std::string> embeddingModelTags;

class AutoEmbeddingModel {
protected:
	std::string model_path = "";
	std::unique_ptr<embedding_model> embedding_model_impl = nullptr;
	std::unique_ptr<Tokenizer> tokenizer = nullptr;
	std::unique_ptr<Q4NX> q4nx = nullptr;
	bool is_model_loaded = false;
	std::string current_model = "";
	xrt::device* npu_device_inst = nullptr;
	std::unique_ptr<npu_xclbin_manager> npu = nullptr;
	bool enable_preemption = false;

	std::unique_ptr<LM_Config> lm_config = nullptr;


	void _shared_load_model(std::string model_path, json model_info, bool enable_preemption = false);
	nlohmann::json _shared_setup_tokenizer(std::string model_path);

	buffer<bf16> _shared_embed(std::vector<int>& tokens);

public:
	//************ Shared by all models *************/
	virtual ~AutoEmbeddingModel() = default;

	AutoEmbeddingModel(xrt::device* npu_device_inst, std::string current_model = "");
	/// \brief Get the current model
	/// \return the current model
	std::string get_current_model();

	/// \brief Show the model info
	/// \return the model info
	std::string show_model_info();

	//************ Unique for each model *************/

	virtual void load_model(std::string model_path, json model_info, bool enable_preemption) {}
	virtual std::vector<float> embed(std::string& text, embedding_task_type_t task_type) = 0;
};
