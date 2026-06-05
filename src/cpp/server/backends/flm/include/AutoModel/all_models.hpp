/// \file all_models.hpp
/// \brief all_models class
/// \author FastFlowLM Team
/// \date 2025-09-10
/// \version 0.9.24
/// \note This is a header file for the all_models class
#pragma once

#include "modeling_llama3.hpp"
#include "modeling_gemma3.hpp"
#include "modeling_gemma3_text.hpp"
#include "modeling_qwen3.hpp"
#include "modeling_gpt_oss.hpp"
#include "modeling_lfm2.hpp"
#include "modeling_phi4.hpp"
#include "modeling_qwen2.hpp"
#include "modeling_qwen3.hpp"
#include "modeling_qwen2vl.hpp"
#include "modeling_qwen3vl.hpp"
#include "modeling_qwen3_5vl.hpp"
#include "modeling_nanbeige.hpp"
#include "modeling_gemma4e.hpp"
#include "model_list.hpp"
#include <nlohmann/json.hpp>

typedef enum {
    llama3,
    deepseek_r1,
    deepseek_r1_0528,
    qwen2,
    qwen2vl,
    qwen3,
    qwen3_it,
    qwen3_tk,
    qwen3vl,
    qwen3_5,
    gemma3,
    gemma3_text,
    gemma4e,
    gpt_oss,
    lfm2,
    lfm2_5_tk,
    phi4,
    nanbeige,
    error_whiper,
    error_embedding
} SupportedModelFamily;

inline std::pair<std::string, std::unique_ptr<AutoModel>> get_auto_model(const std::string& model_tag, model_list& available_models, xrt::device* npu_device_inst) {


    static const std::map<std::string, SupportedModelFamily> modelFamilyMap = {
        {"llama3", SupportedModelFamily::llama3},
        {"deepseek-r1", SupportedModelFamily::deepseek_r1},
        {"deepseek-r1-0528", SupportedModelFamily::deepseek_r1_0528},
        {"qwen2", SupportedModelFamily::qwen2},
        {"qwen3", SupportedModelFamily::qwen3},
        {"qwen3-it", SupportedModelFamily::qwen3_it},
        {"qwen3-tk", SupportedModelFamily::qwen3_tk},
        {"qwen3vl", SupportedModelFamily::qwen3vl},
        {"qwen3.5", SupportedModelFamily::qwen3_5},
        {"gemma3", SupportedModelFamily::gemma3},
        {"gemma3-text", SupportedModelFamily::gemma3_text},
        {"gemma4e", SupportedModelFamily::gemma4e},
        {"gpt-oss", SupportedModelFamily::gpt_oss},
        {"lfm2", SupportedModelFamily::lfm2},
        {"lfm2.5-tk", SupportedModelFamily::lfm2_5_tk},
        {"qwen2vl", SupportedModelFamily::qwen2vl},
        {"phi4", SupportedModelFamily::phi4},
        {"nanbeige", SupportedModelFamily::nanbeige},
        {"whisper-v3", SupportedModelFamily::error_whiper},
        {"embed-gemma", SupportedModelFamily::error_embedding}
    };


    if (available_models.is_model_supported(model_tag) == false) {
        header_print_r("ERROR", "Model tag '" << model_tag << "' is not supported. Please check the model list.");
        return std::make_pair("llama3.2:1b", std::make_unique<Llama3>(npu_device_inst));
    }

    std::unique_ptr<AutoModel> auto_chat_engine = nullptr;
    auto [new_model_tag, model_info] = available_models.get_model_info(model_tag);

    switch(modelFamilyMap.at(model_info["details"]["family"])) {
        case SupportedModelFamily::llama3:
            auto_chat_engine = std::make_unique<Llama3>(npu_device_inst);
            break;
        case SupportedModelFamily::deepseek_r1:
            auto_chat_engine = std::make_unique<DeepSeek_r1_8b>(npu_device_inst);
            break;
        case SupportedModelFamily::deepseek_r1_0528:
            auto_chat_engine = std::make_unique<DeepSeek_r1_0528_8b>(npu_device_inst);
            break;
        case SupportedModelFamily::qwen2:
            auto_chat_engine = std::make_unique<Qwen2>(npu_device_inst);
            break;
        case SupportedModelFamily::qwen2vl:
            auto_chat_engine = std::make_unique<Qwen2VL>(npu_device_inst);
            break;
        case SupportedModelFamily::qwen3:
            auto_chat_engine = std::make_unique<Qwen3>(npu_device_inst);
            break;
        case SupportedModelFamily::qwen3_it:
            auto_chat_engine = std::make_unique<Qwen3_IT>(npu_device_inst);
            break;
        case SupportedModelFamily::qwen3_tk:
            auto_chat_engine = std::make_unique<Qwen3_TK>(npu_device_inst);
            break;
        case SupportedModelFamily::gemma3:
            auto_chat_engine = std::make_unique<Gemma3>(npu_device_inst);
            break;
        case SupportedModelFamily::gemma3_text:
            auto_chat_engine = std::make_unique<Gemma3_Text_Only>(npu_device_inst);
            break;
        case SupportedModelFamily::gemma4e:
            auto_chat_engine = std::make_unique<Gemma4e>(npu_device_inst);
            break;
        case SupportedModelFamily::gpt_oss:
            auto_chat_engine = std::make_unique<GPT_OSS>(npu_device_inst);
            break;
        case SupportedModelFamily::qwen3vl:
            auto_chat_engine = std::make_unique<Qwen3VL>(npu_device_inst);
            break;
        case SupportedModelFamily::qwen3_5:
            auto_chat_engine = std::make_unique<Qwen3_5VL>(npu_device_inst);
            break;
        case SupportedModelFamily::lfm2:
            auto_chat_engine = std::make_unique<LFM2>(npu_device_inst);
            break;
        case SupportedModelFamily::lfm2_5_tk:
            auto_chat_engine = std::make_unique<LFM2_5_TK>(npu_device_inst);
            break;
        case SupportedModelFamily::nanbeige:
            auto_chat_engine = std::make_unique<Nanbeige>(npu_device_inst);
            break;
        case SupportedModelFamily::phi4:
            auto_chat_engine = std::make_unique<Phi4>(npu_device_inst);
            break;
        case SupportedModelFamily::error_whiper:
        case SupportedModelFamily::error_embedding:
        default:
            header_print_r("ERROR", "Unsupported model family or non-llm: " << model_info["details"]["family"]);
            auto_chat_engine = std::make_unique<Llama3>(npu_device_inst);
            new_model_tag = "llama3.2:1b";
    }

    return std::make_pair(new_model_tag, std::move(auto_chat_engine));
}
