/// \file lfm2.hpp
/// \brief lfm2 class
/// \author FastFlowLM Team
/// \date 2025-11-03
/// \version 0.9.15
/// \note This is a source file for the lfm2 class

#pragma once
#include "AutoModel/automodel.hpp"

/************              LFM family            **************/
class LFM2 : public AutoModel {
private:
    void setup_tokenizer(std::string model_path);
    inline std::string _replace_space(std::string& text){
        static std::string to_replace = "Ġ";
        static std::string replace_with = " ";
        size_t pos = 0;
        while ((pos = text.find(to_replace, pos)) != std::string::npos) {
            text.replace(pos, to_replace.length(), replace_with);
            pos += replace_with.length();
        }
        return text;
    }

public:
    LFM2(xrt::device* npu_device_inst);

    void load_model(std::string model_path, json model_inf, int default_context_length = -1, bool enable_preemption = false) override;
    //void toggle_enable_think() override;
    bool insert(chat_meta_info_t& meta_info, lm_uniform_input_t& input, std::function<bool()> is_cancelled = [] { return false; }) override;
    std::string generate(chat_meta_info_t& meta_info, int length_limit, std::ostream& os, std::function<bool()> is_cancelled = [] { return false; }) override;
    std::string generate_with_prompt(chat_meta_info_t& meta_info, lm_uniform_input_t& input, int length_limit, std::ostream& os = std::cout) override;
    std::string apply_chat_template(nlohmann::ordered_json& messages, nlohmann::ordered_json tools = nlohmann::ordered_json::object()) override;
    StreamResult parse_stream_content(const std::string content);
};

class LFM2_5_TK : public AutoModel {
private:
    int think_start_id = 64400;
    int think_end_id = 64401;

    void setup_tokenizer(std::string model_path);
    inline std::string _replace_space(std::string& text){
        static std::string to_replace = "Ġ";
        static std::string replace_with = " ";
        size_t pos = 0;
        while ((pos = text.find(to_replace, pos)) != std::string::npos) {
            text.replace(pos, to_replace.length(), replace_with);
            pos += replace_with.length();
        }
        return text;
    }

public:
    LFM2_5_TK(xrt::device* npu_device_inst);

    void load_model(std::string model_path, json model_inf, int default_context_length = -1, bool enable_preemption = false) override;
    //void toggle_enable_think() override;
    bool insert(chat_meta_info_t& meta_info, lm_uniform_input_t& input, std::function<bool()> is_cancelled = [] { return false; }) override;
    std::string generate(chat_meta_info_t& meta_info, int length_limit, std::ostream& os, std::function<bool()> is_cancelled = [] { return false; }) override;
    std::string generate_with_prompt(chat_meta_info_t& meta_info, lm_uniform_input_t& input, int length_limit, std::ostream& os = std::cout) override;
    std::string apply_chat_template(nlohmann::ordered_json& messages, nlohmann::ordered_json tools = nlohmann::ordered_json::object()) override;
    NonStreamResult parse_nstream_content(const std::string response_text);
    StreamResult parse_stream_content(const std::string content);
};
