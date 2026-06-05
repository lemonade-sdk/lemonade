/// \file modeling_gpt_oss.hpp
/// \brief modeling_gpt_oss class
/// \author FastFlowLM Team
/// \date 2025-10-01
/// \version 0.9.24
/// \note This is a source file for the gpt-oss class
#pragma once

#include "AutoModel/automodel.hpp"

class GPT_OSS : public AutoModel {
private:

    bool enable_think = true;

    std::string reasoning_effort = "low";
    std::string model_identity = "You are ChatGPT, a large language model trained by OpenAI.";
    std::string role = "developer";

    void setup_tokenizer(std::string model_path);

    enum ToolGrammarState {
        STATE_EXPECT_TO_FUNCTIONS,
        STATE_EXPECT_FUNCTION_NAME,
        STATE_EXPECT_CONSTRAIN,
        STATE_EXPECT_MESSAGE,
        STATE_COMPLETE
    };
    ToolGrammarState current_state = STATE_EXPECT_TO_FUNCTIONS;
    std::string accumulated_text;
    void reset_tool_grammar_state() {
        current_state = STATE_EXPECT_TO_FUNCTIONS;
        accumulated_text.clear();
    }
public:
    GPT_OSS(xrt::device* npu_device_inst);

    void load_model(std::string model_path, json model_info, int default_context_length = -1, bool enable_preemption = false) override;
    std::string apply_chat_template(nlohmann::ordered_json& messages, nlohmann::ordered_json tools = nlohmann::ordered_json::object()) override;
    bool insert(chat_meta_info_t& meta_info, lm_uniform_input_t& input, std::function<bool()> is_cancelled = [] { return false; }) override;
    std::string generate(chat_meta_info_t& meta_info, int length_limit, std::ostream& os, std::function<bool()> is_cancelled = [] { return false; }) override;
    std::string generate_with_prompt(chat_meta_info_t& meta_info, lm_uniform_input_t& input, int length_limit, std::ostream& os = std::cout) override;
    int _sample_in_tool(buffer<bf16>& logits);
    std::vector<int> get_to_functions_tokens();
    std::vector<int> get_function_name_tokens();
    void update_state(int token_id);
    void mask_logits(buffer<bf16>& logits, const std::vector<int>& allowed_tokens);
    NonStreamResult parse_nstream_content(const std::string response_text);
    StreamResult parse_stream_content(const std::string content);
    chat_template_type_t get_chat_template_type() {
        return chat_template_type_t::harmony;
    }

    /// \brief Override configure_parameter to handle GPT-oss-specific parameters
    bool configure_parameter(std::string parameter_name, const std::any& value) override {
        if (parameter_name == "reasoning_effort") {
            try {
                this->reasoning_effort = std::any_cast<std::string>(value);
                if (this->reasoning_effort == "high" || this->reasoning_effort == "medium" || this->reasoning_effort == "low")
                    this->extra_context["reasoning_effort"] = this->reasoning_effort;
                else
                    header_print("WARNING", "Reasoning effort must be 'low', 'medium' or 'high'!");
                return true;
            }
            catch (const std::bad_any_cast&) {
                return false;
            }
        }
        else if (parameter_name == "system_prompt") {
            try {
                this->user_system_prompt = std::any_cast<std::string>(value);
                this->extra_context["user_system_prompt"] = this->user_system_prompt;
                return true;
            }
            catch (const std::bad_any_cast&) {
                return false;
            }
        }
        // Call base class implementation for any unhandled parameters
        return AutoModel::configure_parameter(parameter_name, value);
    }
};
