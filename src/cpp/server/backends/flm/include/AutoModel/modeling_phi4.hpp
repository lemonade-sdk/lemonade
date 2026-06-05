/// \file phi4.hpp
/// \brief phi4 class
/// \author FastFlowLM Team
/// \date 2025-09-04
/// \version 0.9.25
/// \note This is a source file for the phi4 class
#pragma once
#include "AutoModel/automodel.hpp"

/************              phi4 family            **************/
class Phi4 : public AutoModel {
private:
    void setup_tokenizer(std::string model_path);

public:
    Phi4(xrt::device* npu_device_inst);

    void load_model(std::string model_path, json model_inf, int default_context_length = -1, bool enable_preemption = false) override;
    //void toggle_enable_think() override;
    bool insert(chat_meta_info_t& meta_info, lm_uniform_input_t& input, std::function<bool()> is_cancelled = [] { return false; }) override;
    std::string generate(chat_meta_info_t& meta_info, int length_limit, std::ostream& os, std::function<bool()> is_cancelled = [] { return false; }) override;
    std::string generate_with_prompt(chat_meta_info_t& meta_info, lm_uniform_input_t& input, int length_limit, std::ostream& os = std::cout) override;
    std::string apply_chat_template(nlohmann::ordered_json& messages, nlohmann::ordered_json tools = nlohmann::ordered_json::object()) override;
};
