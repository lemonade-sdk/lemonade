/// \file llama3.hpp
/// \brief llama3 class
/// \author FastFlowLM Team
/// \date 2025-09-04
/// \version 0.9.24
/// \note This is a source file for the llama3 class

#pragma once
#include "AutoModel/automodel.hpp"

/************              llama family            **************/
class Llama3 : public AutoModel {
private:
    void setup_tokenizer(std::string model_path);

public:
    Llama3(xrt::device* npu_device_inst);

    void load_model(std::string model_path, json model_inf, int default_context_length = -1, bool enable_preemption = false) override;
    //void toggle_enable_think() override;
    bool insert(chat_meta_info_t& meta_info, lm_uniform_input_t& input, std::function<bool()> is_cancelled = [] { return false; }) override;
    std::string generate(chat_meta_info_t& meta_info, int length_limit, std::ostream& os, std::function<bool()> is_cancelled = [] { return false; }) override;
    std::string generate_with_prompt(chat_meta_info_t& meta_info, lm_uniform_input_t& input, int length_limit, std::ostream& os = std::cout) override;
    std::string apply_chat_template(nlohmann::ordered_json& messages, nlohmann::ordered_json tools = nlohmann::ordered_json::object()) override;
};

/************              DeepSeek_r1_8b            **************/
class DeepSeek_r1_8b : public AutoModel {
private:
    std::string current_model = "DeepSeek_r1_8b";
    int think_marker_id;
    int think_start_id = 128013; // placeholder token id for "<think>"
    int think_end_id = 128014;   // placeholder token id for "</think>"
    int newline_id = 198;        // token id for "\n"
    int double_newline_id = 271; // token id for "\n\n"
    void setup_tokenizer(std::string model_path);

public:
    DeepSeek_r1_8b(xrt::device* npu_device_inst);

    void load_model(std::string model_path, json model_inf, int default_context_length = -1, bool enable_preemption = false) override;
    //void toggle_enable_think() override;
    bool insert(chat_meta_info_t& meta_info, lm_uniform_input_t& input, std::function<bool()> is_cancelled = [] { return false; }) override;
    std::string generate(chat_meta_info_t& meta_info, int length_limit, std::ostream& os, std::function<bool()> is_cancelled = [] { return false; }) override;
    std::string generate_with_prompt(chat_meta_info_t& meta_info, lm_uniform_input_t& input, int length_limit, std::ostream& os = std::cout) override;
    std::string apply_chat_template(nlohmann::ordered_json& messages, nlohmann::ordered_json tools = nlohmann::ordered_json::object()) override;
    NonStreamResult parse_nstream_content(const std::string response_text);
    StreamResult parse_stream_content(const std::string content);
};
