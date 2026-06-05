/// \file gemma3.hpp
/// \brief gemma3 class
/// \author FastFlowLM Team
/// \date 2025-09-03
/// \version 0.9.24
/// \note This is a source file for the gemma3 class

#pragma once
#include "AutoModel/automodel.hpp"
#include "base64.hpp"
#include "image/image_reader.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>


/************              Gemma3_4b            **************/
class Gemma3 : public AutoModel {
private:

    void setup_tokenizer(std::string model_path);

    // Image processing functionality
    ImageReader image_reader_;
    bytes load_image(const std::string& filename);
    bytes load_image_base64(const std::string& base64_string);
    buffer<bf16> preprocess_image(bytes& image);

public:
    Gemma3(xrt::device* npu_device_inst);

    void load_model(std::string model_path, json model_inf, int default_context_length = -1, bool enable_preemption = false) override;
    //void toggle_enable_think() override;
    bool insert(chat_meta_info_t& meta_info, lm_uniform_input_t& input, std::function<bool()> is_cancelled = [] { return false; }) override;
    std::string generate(chat_meta_info_t& meta_info, int length_limit, std::ostream& os, std::function<bool()> is_cancelled = [] { return false; }) override;
    std::string generate_with_prompt(chat_meta_info_t& meta_info, lm_uniform_input_t& input, int length_limit, std::ostream& os = std::cout) override;
    std::string apply_chat_template(nlohmann::ordered_json& messages, nlohmann::ordered_json tools = nlohmann::ordered_json::object()) override;
};
