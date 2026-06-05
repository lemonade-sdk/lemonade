/// \file modeling_gpt_oss.cpp
/// \brief modeling_gpt_oss class
/// \author FastFlowLM Team
/// \date 2025-10-01
/// \version 0.9.24
/// \note This is a source file for the gpt-oss class
#include "AutoModel/modeling_gpt_oss.hpp"


GPT_OSS::GPT_OSS(xrt::device* npu_device_inst) : AutoModel(npu_device_inst, "gpt-oss") {}

void GPT_OSS::load_model(std::string model_path, json model_info, int default_context_length, bool enable_preemption) {
    this->model_path = model_path;
    this->_shared_load_model(model_path, model_info, default_context_length, enable_preemption);
    this->q4nx = std::make_unique<Q4NX>(this->model_path);
    this->lm_engine = std::make_unique<gpt_oss_npu>(*this->lm_config, this->npu.get(), this->MAX_L);
    this->lm_engine->load_weights(*this->q4nx);
    this->q4nx.reset();
    this->tokenizer = std::make_unique<Tokenizer>(model_path);

    this->setup_tokenizer(model_path);
    this->sampler.reset();

    sampler_config config;
    config.top_k = 10;
    config.top_p = 0.95;
    config.min_p = 0.1;
    config.temperature = 0.6;
    config.rep_penalty = 1.05;
    config.freq_penalty = 1.05;

    this->set_sampler(config);
    for (size_t i = 0; i < PROFILER_TYPE_NUM; i++) {
        this->profiler_list[i].reset();
    }
}

void GPT_OSS::setup_tokenizer(std::string model_path){
    auto tokenizer_config = this->_shared_setup_tokenizer(model_path);
}

std::string GPT_OSS::apply_chat_template(nlohmann::ordered_json& messages, nlohmann::ordered_json tools) {
    minja::chat_template_inputs inputs;
    inputs.add_generation_prompt = true;
    inputs.messages = messages;
    inputs.extra_context = this->extra_context;
    inputs.extra_context["enable_thinking"] = this->enable_think;
    inputs.extra_context["reasoning_effort"] = this->reasoning_effort;
    inputs.extra_context["model_identity"] = this->model_identity;
    inputs.extra_context["role"] = this->role;
    //inputs.tools = tools;

    return this->chat_tmpl->apply(inputs);
}
json tools;
bool GPT_OSS::insert(chat_meta_info_t& meta_info, lm_uniform_input_t& input, std::function<bool()> is_cancelled)
{
    // preprocess
    this->profiler_list[TKOEN_ENCODE_TIME].start();
    std::string templated_text;
    if (input.messages.empty() && input.prompt.empty()) {
        header_print("WARNING", "No messages or prompt provided");
        return false;
    }
    if (!input.messages.empty()) { // already a formated messages, usually from REST API
        tools = input.tools;
        templated_text = this->apply_chat_template(input.messages);
        //templated_text = this->apply_chat_template(input.messages, input.tools);
    }
    else if (!input.prompt.empty()) { // a pure text, usually from the cli
        nlohmann::ordered_json messages;

        messages.push_back({ {"role", "user"}, {"content", input.prompt} });
        templated_text = this->apply_chat_template(messages);
    }

    std::vector<int> tokens = this->tokenizer->encode(templated_text);

    this->profiler_list[TKOEN_ENCODE_TIME].stop(tokens.size());

    // hardware
    int restore_idx = -1;
    gpt_oss_npu *gpt_oss_engine = dynamic_cast<gpt_oss_npu*>(this->lm_engine.get());
    if (meta_info.restore_allowed) {
        restore_idx = gpt_oss_engine->restore();
        this->total_tokens = restore_idx;
        this->token_history = checkpoint_his; // restore the token history to be consistent with the restored KV cache, which is crucial for correct functioning of _shared_insert's prefix-matching logic
    }
    bool success = this->_shared_insert(meta_info, tokens, is_cancelled, nullptr);

    checkpoint_his = token_history;
    int checkpoint_idx = gpt_oss_engine->checkpoint();

    return success;
}

std::string GPT_OSS::generate(chat_meta_info_t& meta_info, int length_limit, std::ostream& os, std::function<bool()> is_cancelled) {
    os << "<|start|>" << std::flush;
    os << "assistant" << std::flush;
    std::vector<int> sampled_tokens;
    std::string result;
    if (length_limit > 0){
        sampled_tokens.reserve(length_limit);
    }
    else{
        sampled_tokens.reserve(4096);
    }
    assert(this->last_token != -1);

    stop_reason_t reason = EOT_DETECTED;
    int last_sampled_token = this->last_token;


    token_history.push_back(last_token);
    if (this->is_normal_token(last_sampled_token) && last_sampled_token != -1){
        std::string token_str = this->tokenizer->run_time_decoder(last_sampled_token);
        result += token_str;
        os << token_str << std::flush;

    }
    if (this->is_eos(last_sampled_token)){
        return result;
    }
    this->profiler_list[DECODING_TIME].reset();
    this->profiler_list[TKOEN_DECODE_TIME].reset();
    if (this->total_tokens >= this->MAX_L){
        header_print("WARNING", "Max length reached, stopping generation...");
        reason = MAX_LENGTH_REACHED;
        return result;
    }
    while (this->total_tokens < this->MAX_L){
        if (is_cancelled()) {
            reason = CANCEL_DETECTED;
            // reset stream content
            buffer_.clear();
            current_mode_ = StreamEventType::CONTENT;
            tool_name_.clear();
            is_in_tool_block_ = false;
            break;
        }
        this->profiler_list[DECODING_TIME].start();
        buffer<bf16> y = this->lm_engine->forward(last_sampled_token);
        this->profiler_list[DECODING_TIME].stop(1);

        this->profiler_list[SAMPLING_TIME].start();
        int sampled_token = this->sampler->sample(y);
        this->profiler_list[SAMPLING_TIME].stop(1);
        this->total_tokens++;
        last_sampled_token = sampled_token;

        this->profiler_list[TKOEN_DECODE_TIME].start();
        if (this->is_normal_token(sampled_token)){ // filter out special tokens
            std::string token_str = this->tokenizer->run_time_decoder(sampled_token);
            os << token_str << std::flush;
            result += token_str;
        }
        this->profiler_list[TKOEN_DECODE_TIME].stop(1);
        token_history.push_back(sampled_token);
        if (this->is_eos(sampled_token)){
            meta_info.generated_tokens++;
            this->lm_engine->forward(last_sampled_token);
            break;
        }
        meta_info.generated_tokens++;
        if ((length_limit > 0) && (meta_info.generated_tokens >= length_limit)){
            reason = MAX_LENGTH_REACHED;
            break;
        }
    }
    meta_info.decoding_duration = (uint64_t)(time_utils::cast_to_us(this->profiler_list[DECODING_TIME].get_total_time()).first) * 1e3;
    meta_info.stop_reason = reason;
    if (this->total_tokens >= this->MAX_L){
        header_print("WARNING", "Max length reached, stopping generation...");
    }
    std::cout << std::endl;
    header_print("FLM", "Model RAW Output: \n" + result);

    os << "<|end|>" << std::flush;
    return "<|start|>assistant" + result + "<|end|>";
}

/*
// std::string GPT_OSS::generate(chat_meta_info_t& meta_info, int length_limit, std::ostream& os, std::function<bool()> is_cancelled) {
//     const std::string MARKER_TOOL_START = "<|start|>assistant<|channel|>commentary";
//     bool is_constrained_mode = false;

//     os << "<|start|>" << std::flush;
//     os << "assistant" << std::flush;
//     std::string result = this->_shared_generate(meta_info, length_limit, os, is_cancelled);
//     //std::vector<int> sampled_tokens;
//     //std::string result;
//     //if (length_limit > 0) {
//     //    sampled_tokens.reserve(length_limit);
//     //}
//     //else {
//     //    sampled_tokens.reserve(4096);
//     //}
//     //assert(this->last_token != -1);

//     //stop_reason_t reason = EOT_DETECTED;
//     //int last_sampled_token = this->last_token;
//     //this->token_history.push_back(this->last_token);
//     //auto decoding_start_time = time_utils::now();
//     //if (this->is_normal_token(last_sampled_token) && last_sampled_token != -1) {
//     //    std::string token_str = this->tokenizer->run_time_decoder(last_sampled_token);
//     //    result += token_str;
//     //    os << token_str << std::flush;

//     //}
//     //if (this->is_eos(last_sampled_token)) {
//     //    return result;
//     //}
//     //this->profiler_list[TKOEN_DECODE_TIME].stop(1);
//     //if (this->total_tokens >= this->MAX_L) {
//     //    header_print("WARNING", "Max length reached, stopping generation...");
//     //    reason = MAX_LENGTH_REACHED;
//     //    return result;
//     //}
//     //while (this->total_tokens < this->MAX_L) {
//     //    if (is_cancelled()) {
//     //        reason = CANCEL_DETECTED;
//     //        // reset stream content
//     //        buffer_.clear();
//     //        current_mode_ = StreamEventType::CONTENT;
//     //        waiting_for_header_ = true;
//     //        break;
//     //    }
//     //    this->profiler_list[DECODING_TIME].start();
//     //    buffer<bf16> y = this->lm_engine->forward(last_sampled_token);
//     //    this->profiler_list[DECODING_TIME].stop(1);

//     //    this->profiler_list[SAMPLING_TIME].start();
//     //    // if MARKER_TOOL_START found
//     //
//     //    if (!is_constrained_mode) {
//     //        if (result.length() >= MARKER_TOOL_START.length() &&
//     //            result.compare(result.length() - MARKER_TOOL_START.length(), MARKER_TOOL_START.length(), MARKER_TOOL_START) == 0) {
//     //            is_constrained_mode = true;
//     //            reset_tool_grammar_state();
//     //            header_print("INFO", "<|start|>assistant<|channel|>commentary detected! Switching to Constrained Decoding.");
//     //        }
//     //    }

//     //    int sampled_token;
//     //    if (is_constrained_mode) {
//     //        sampled_token = _sample_in_tool(y);
//     //        if (current_state == STATE_COMPLETE) {
//     //            is_constrained_mode = false;
//     //            header_print("INFO", "Constrained Decoding complete, returning to normal mode");
//     //        }
//     //    }
//     //    else
//     //        sampled_token = this->sampler->sample(y);
//     //    this->profiler_list[SAMPLING_TIME].stop(1);
//     //    this->total_tokens++;
//     //    last_sampled_token = sampled_token;

//     //    this->profiler_list[TKOEN_DECODE_TIME].start();
//     //    this->profiler_list[TKOEN_DECODE_TIME].stop(1);
//     //    if (this->is_normal_token(sampled_token)) { // filter out special tokens
//     //        std::string token_str = this->tokenizer->run_time_decoder(sampled_token);
//     //        os << token_str << std::flush;
//     //        result += token_str;
//     //    }
//     //    this->token_history.push_back(sampled_token);
//     //    if (this->is_eos(sampled_token)) {
//     //        this->lm_engine->forward(last_sampled_token);
//     //        break;
//     //    }
//     //    meta_info.generated_tokens++;
//     //    if ((length_limit > 0) && (meta_info.generated_tokens >= length_limit)) {
//     //        reason = MAX_LENGTH_REACHED;
//     //        break;
//     //    }
//     //}

//     //auto decoding_end_time = time_utils::now();
//     //meta_info.decoding_duration = (uint64_t)time_utils::duration_ns(decoding_start_time, decoding_end_time).first;
//     //meta_info.stop_reason = reason;
//     //if (this->total_tokens >= this->MAX_L) {
//     //    header_print("WARNING", "Max length reached, stopping generation...");
//     //}

//     os << "<|end|>" << std::flush;
//     return "<|start|>assistant" + result + "<|end|>";
// }
*/

std::string GPT_OSS::generate_with_prompt(chat_meta_info_t& meta_info, lm_uniform_input_t& input, int length_limit, std::ostream& os) {
    if (!this->insert(meta_info, input)) {
        return "";
    }
    os << "<|start|>" << std::flush;
    os << "assistant" << std::flush;
    std::string result = this->_shared_generate(meta_info, length_limit, os);
    os << "<|end|>" << std::flush;
    return "<|start|>assistant" + result + "<|end|>";
}

NonStreamResult GPT_OSS::parse_nstream_content(const std::string response_text) {
    NonStreamResult result;

    const std::string think_start_tag = "<|start|>assistant<|channel|>analysis<|message|>";
    const std::string think_end_tag = "<|end|>";
    const std::string final_start_tag = "<|start|>assistant<|channel|>final<|message|>";
    const std::string final_end_tag = "<|end|>";

    // --- Parse reasoning content ---
    size_t t_start = response_text.find(think_start_tag);
    size_t t_end = response_text.find(think_end_tag, t_start + think_start_tag.size());

    if (t_start != std::string::npos && t_end != std::string::npos) {
        t_start += think_start_tag.size();
        result.reasoning_content = response_text.substr(t_start, t_end - t_start);
    }

    // --- Parse final content ---
    size_t f_start = response_text.find(final_start_tag);
    size_t f_end = response_text.find(final_end_tag, f_start + final_start_tag.size());

    if (f_start != std::string::npos && f_end != std::string::npos) {
        f_start += final_start_tag.size();
        result.content = response_text.substr(f_start, f_end - f_start);
    }

    return result;
}

int GPT_OSS::_sample_in_tool(buffer<bf16>& logits) {
    // to=functions.get_current_weather <|constrain|>json<|message|>{"location":"San Francisco"}<|call|>
    std::vector<int> allowed_tokens;
    switch (current_state) {
        case STATE_EXPECT_TO_FUNCTIONS: {
            allowed_tokens = get_to_functions_tokens();
            break;
        }
        case STATE_EXPECT_FUNCTION_NAME: {
            allowed_tokens = get_function_name_tokens();
            break;
        }
        case STATE_EXPECT_CONSTRAIN: {
            auto constrain_tokens = tokenizer->encode(" <|constrain|>");
            auto message_tokens = tokenizer->encode("json");
            allowed_tokens.insert(allowed_tokens.end(),
                constrain_tokens.begin(), constrain_tokens.end());
            allowed_tokens.insert(allowed_tokens.end(),
                message_tokens.begin(), message_tokens.end());
            break;
        }
        case STATE_EXPECT_MESSAGE: {
            auto tokens = tokenizer->encode("<|message|>");
            allowed_tokens = { tokens[0] };
            break;
        }
        //case STATE_IN_JSON: {
        //    header_print("JSONSSS", "SSSS");
        //    return this->sampler->sample(logits);
        //    //allowed_tokens = constrain_by_json_schema(logits);
        //    //break;
        //}
    }
    mask_logits(logits, allowed_tokens);
    int token_id = this->sampler->sample(logits);
    update_state(token_id);
    return token_id;
    //return this->sampler->sample(logits);
}

std::vector<int> GPT_OSS::get_to_functions_tokens() {
    const std::string target = " to=functions.";

    std::string remaining = target.substr(accumulated_text.length());

    auto remaining_tokens = tokenizer->encode(remaining);

    if (remaining_tokens.empty()) {
        return {};
    }

    return { remaining_tokens[0] };
}

std::vector<int> GPT_OSS::get_function_name_tokens() {
    std::vector<int> allowed;

    for (const auto& tool : tools) {
        std::string name = tool["function"]["name"];

        auto tokens = this->tokenizer->encode(name);
        for (int token : tokens) {
            allowed.push_back(token);
        }
    }

    return allowed;
}

void GPT_OSS::update_state(int token_id) {
    std::string token_str = tokenizer->decode({ token_id });
    accumulated_text += token_str;

    switch (current_state) {
        case STATE_EXPECT_TO_FUNCTIONS: {
            if (accumulated_text.find("to=functions.") != std::string::npos) {
                current_state = STATE_EXPECT_FUNCTION_NAME;
                accumulated_text.clear();
                header_print("TOOLCALLING", "Completed 'to=functions.', expecting function name");
            }
            break;
        }
        case STATE_EXPECT_FUNCTION_NAME: {
            //bool found = false;
            for (const auto& tool : tools) {
                std::string name = tool["function"]["name"];
                if (accumulated_text == name) {
                    current_state = STATE_EXPECT_CONSTRAIN;
                    accumulated_text.clear();
                    //found = true;
                    header_print("TOOLCALLING", "Matched function: " + name);
                    break;
                }
            }
            break;
        }
        case STATE_EXPECT_CONSTRAIN: {
            if (token_str.find("json") != std::string::npos) {
                current_state = STATE_EXPECT_MESSAGE;
                accumulated_text.clear();
                header_print("TOOLCALLING", "Starting MESSAGE generation");
            }
            break;
        }
        case STATE_EXPECT_MESSAGE: {
            if (token_str.find("<|message|>") != std::string::npos) {
                current_state = STATE_COMPLETE;
                accumulated_text.clear();
            }
            break;
        }
    }
}

void GPT_OSS::mask_logits(buffer<bf16>& logits, const std::vector<int>& allowed_tokens) {
    const float NEG_INF = -std::numeric_limits<float>::infinity();

    std::unordered_set<int> allowed_set(allowed_tokens.begin(), allowed_tokens.end());

    for (int i = 0; i < logits.size(); i++) {
        if (allowed_set.find(i) == allowed_set.end()) {
            logits[i] = static_cast<bf16>(NEG_INF);
        }
    }
}

StreamResult GPT_OSS::parse_stream_content(const std::string content) {
    //header_print("GPTOSSHERE", content);

    const std::string MARKER_REASONING = "<|start|>assistant<|channel|>analysis<|message|>";
    const std::string MARKER_NORMAL = "<|start|>assistant<|channel|>final<|message|>";
    const std::string MARKER_END = "<|end|>";

    // Markers for Tool Calling
    const std::string MARKER_TOOL_START = "<|start|>assistant<|channel|>commentary to=functions.";
    //const std::string MARKER_TOOL_START = "<|start|>assistant<|channel|>commentary <|constrain|>functions.";
    const std::string MARKER_TOOL_SPLIT = "<|message|>";
    const std::string MARKER_TOOL_END = "<|end|>";

    StreamResult result;
    buffer_ += content; // Append new chunk to buffer

    // for test
    //result.content = content;
    //result.type = current_mode_;
    //return result;
    // for test

    while (true) {
        if (waiting_for_header_) {
            size_t pos_reason = buffer_.find(MARKER_REASONING);
            size_t pos_normal = buffer_.find(MARKER_NORMAL);
            size_t pos_tool = buffer_.find(MARKER_TOOL_START);

            // Find which marker appears first
            size_t pos_first = std::string::npos;
            StreamEventType next_mode = StreamEventType::REASONING; // default
            size_t marker_len = 0;

            if (pos_reason != std::string::npos) {
                pos_first = pos_reason;
                next_mode = StreamEventType::REASONING;
                marker_len = MARKER_REASONING.length();
            }
            if (pos_normal != std::string::npos && (pos_first == std::string::npos || pos_normal < pos_first)) {
                pos_first = pos_normal;
                next_mode = StreamEventType::CONTENT;
                marker_len = MARKER_NORMAL.length();
            }
            if (pos_tool != std::string::npos && (pos_first == std::string::npos || pos_tool < pos_first)) {
                pos_first = pos_tool;
                next_mode = StreamEventType::WAITING; // Special mode for buffering tool
                marker_len = MARKER_TOOL_START.length();
            }

            // found
            if (pos_first != std::string::npos) {
                waiting_for_header_ = false;
                current_mode_ = next_mode;

                // Remove garbage before the marker and the marker itself
                buffer_.erase(0, pos_first + marker_len);
                continue;
            }
            else {
                result.type = StreamEventType::WAITING;
                return result;
            }
        }


        if (current_mode_ == StreamEventType::WAITING) {
            size_t pos_end = buffer_.find(MARKER_TOOL_END);

            if (pos_end != std::string::npos) {
                // Format: Name <|constrain|>... <|message|> {JSON}
                std::string full_tool_str = buffer_.substr(0, pos_end);

                size_t pos_split = full_tool_str.find(MARKER_TOOL_SPLIT);
                if (pos_split != std::string::npos) {
                    // 1. Extract Name (Left side)
                    std::string meta_part = full_tool_str.substr(0, pos_split);
                    // Name is typically the first word
                    size_t name_end = meta_part.find_first_of(" <");
                    result.tool_name = meta_part.substr(0, name_end);

                    // 2. Extract JSON (Right side)
                    result.tool_args_str = full_tool_str.substr(pos_split + MARKER_TOOL_SPLIT.length());

                    // 3. Set Result
                    result.type = StreamEventType::TOOL_DONE;
                    result.tool_id = "call_" + std::to_string(std::time(nullptr));
                }
                else {
                    // Fallback if format is wrong
                    result.content = "[Tool Parse Error]";
                    result.type = StreamEventType::CONTENT;
                }

                // Clean up and reset
                buffer_.erase(0, pos_end + MARKER_TOOL_END.length());
                waiting_for_header_ = true;

                // Return immediately, don't stream rest
                return result;
            }
            else {
                // Tool block not finished yet. Wait.
                result.type = StreamEventType::WAITING;
                return result;
            }
        }
        else {
            size_t pos_end = buffer_.find(MARKER_END);

            if (pos_end != std::string::npos) {
                // End marker found
                result.content += buffer_.substr(0, pos_end); // Output content before marker
                result.type = current_mode_;

                // Remove content + marker, ready for next section
                buffer_ = buffer_.substr(pos_end + MARKER_END.length());
                waiting_for_header_ = true; // Go back to waiting for next header
            }
            else {
                // No end marker, flush everything immediately
                result.content += buffer_;
                result.type = current_mode_;
                buffer_.clear();
                break; // Done with this chunk
            }
        }
    }

    return result;
}
