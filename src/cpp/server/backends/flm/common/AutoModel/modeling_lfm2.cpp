/// \file lfm2.cpp
/// \brief lfm2 class
/// \author FastFlowLM Team
/// \date 2025-09-04
/// \version 0.9.15
/// \note This is a source file for the lfm2 class

#include "AutoModel/modeling_lfm2.hpp"
#include "utils/utils.hpp"
#include "metrices.hpp"

/************              LFM2 family            **************/
LFM2::LFM2(xrt::device* npu_device_inst) : AutoModel(npu_device_inst, "LFM2") {}

void LFM2::load_model(std::string model_path, json model_info, int default_context_length, bool enable_preemption) {
    this->_shared_load_model(model_path, model_info, default_context_length, enable_preemption);

    this->q4nx = std::make_unique<Q4NX>(this->model_path);
    // model_type == llama
    this->lm_engine = std::make_unique<lfm2_npu>(*this->lm_config, this->npu.get(), this->MAX_L);

    this->lm_engine->load_weights(*this->q4nx);

    //free the q4nx
    this->q4nx.reset();
    this->lm_engine->clear_context();
    this->setup_tokenizer(model_path);
    this->sampler.reset();

    sampler_config config;
    config.top_p = 0.95;
    config.top_k = 10;
    config.min_p = 0.1;
    config.temperature = 0.3;

    this->set_sampler(config);
    for (size_t i = 0; i < PROFILER_TYPE_NUM; i++) {
        this->profiler_list[i].reset();
    }
}

void LFM2::setup_tokenizer(std::string model_path) {
    auto tokenizer_config = this->_shared_setup_tokenizer(model_path);
    this->tokenizer->is_doubled_encoded = true;
}

std::string LFM2::apply_chat_template(nlohmann::ordered_json& messages, nlohmann::ordered_json tools) {
    minja::chat_template_inputs inputs;
    minja::chat_template_options opt;
    opt.polyfill_tool_responses = false;
    inputs.add_generation_prompt = true;
    inputs.messages = messages;
    inputs.extra_context = this->extra_context;
    // inputs.tools = tools;
    return this->chat_tmpl->apply(inputs, opt);
}

bool LFM2::insert(chat_meta_info_t& meta_info, lm_uniform_input_t& input, std::function<bool()> is_cancelled) {
    // preprocess
    this->profiler_list[TKOEN_ENCODE_TIME].start();
    std::string templated_text;
    if (input.messages.empty() && input.prompt.empty()) {
        header_print("WARNING", "No messages or prompt provided");
        return false;
    }
    if (!input.messages.empty()) { // already a formated messages, usually from REST API
        //templated_text = this->apply_chat_template(input.messages);
        templated_text = this->apply_chat_template(input.messages, input.tools);
    }
    else if (!input.prompt.empty()) { // a pure text, usually from the cli
        nlohmann::ordered_json messages;

        messages.push_back({ {"role", "user"}, {"content", input.prompt} });
        templated_text = this->apply_chat_template(messages);
    }

    std::vector<int> tokens = this->tokenizer->encode(templated_text);

    this->profiler_list[TKOEN_ENCODE_TIME].stop(tokens.size());
    // hardware

    return this->_shared_insert(meta_info, tokens, is_cancelled);
}


std::string LFM2::generate(chat_meta_info_t& meta_info, int length_limit, std::ostream& os, std::function<bool()> is_cancelled) {
    return this->_shared_generate(meta_info, length_limit, os, is_cancelled);
}

std::string LFM2::generate_with_prompt(chat_meta_info_t& meta_info, lm_uniform_input_t& input, int length_limit, std::ostream& os) {
    if (!this->insert(meta_info, input)) {
        return "";
    }
    return this->generate(meta_info, length_limit, os);
}

StreamResult LFM2::parse_stream_content(const std::string content) {
    const std::string TOOL_START = "<|tool_call_start|>";
    const std::string TOOL_END = "<|tool_call_end|>";

    StreamResult result;
    buffer_ += content;

    while (true) {
        if (!is_in_tool_block_) {
            size_t tool_start_pos = buffer_.find(TOOL_START);

            if (tool_start_pos != std::string::npos) {
                if (tool_start_pos > 0) {
                    result.content = buffer_.substr(0, tool_start_pos);
                    result.type = StreamEventType::CONTENT;
                    buffer_ = buffer_.substr(tool_start_pos);
                    return result;
                }

                is_in_tool_block_ = true;
                tool_name_.clear();
                buffer_ = buffer_.substr(TOOL_START.length());
                result.type = StreamEventType::WAITING;
                return result;
            }
            else {
                if (!buffer_.empty()) {
                    result.content = buffer_;
                    result.type = StreamEventType::CONTENT;
                    buffer_.clear();
                    return result;
                }
                break;
            }
        }

        if (is_in_tool_block_) {
            size_t tool_end_pos = buffer_.find(TOOL_END);

            if (tool_end_pos != std::string::npos) {
                tool_name_ += buffer_.substr(0, tool_end_pos);
                buffer_ = buffer_.substr(tool_end_pos + TOOL_END.length());
                is_in_tool_block_ = false;

                std::string raw = tool_name_;

                size_t start = raw.find_first_not_of(" \t\n\r[");
                if (start == std::string::npos) {
                    result.type = StreamEventType::CONTENT;
                    result.content = "[Error: Empty tool call]";
                    return result;
                }
                raw = raw.substr(start);

                size_t pos_open = raw.find('(');
                if (pos_open == std::string::npos) {
                    result.type = StreamEventType::CONTENT;
                    result.content = "[Error: No function bracket found]";
                    return result;
                }

                std::string fn_name = raw.substr(0, pos_open);
                size_t name_end = fn_name.find_last_not_of(" \t\n\r");
                fn_name = fn_name.substr(0, name_end + 1);

                int paren_count = 0;
                size_t pos_close = pos_open;
                for (size_t i = pos_open; i < raw.length(); ++i) {
                    if (raw[i] == '(') paren_count++;
                    if (raw[i] == ')') {
                        paren_count--;
                        if (paren_count == 0) {
                            pos_close = i;
                            break;
                        }
                    }
                }

                std::string args_part = raw.substr(pos_open + 1, pos_close - pos_open - 1);

                json args_json = json::object();

                size_t pos = 0;
                while (pos < args_part.length()) {
                    while (pos < args_part.length() &&
                        (args_part[pos] == ' ' || args_part[pos] == ',' ||
                            args_part[pos] == '\t' || args_part[pos] == '\n')) {
                        pos++;
                    }
                    if (pos >= args_part.length()) break;

                    size_t eq_pos = args_part.find('=', pos);
                    if (eq_pos == std::string::npos) break;

                    std::string key = args_part.substr(pos, eq_pos - pos);
                    size_t key_start = key.find_first_not_of(" \t\n\r");
                    size_t key_end = key.find_last_not_of(" \t\n\r");
                    if (key_start != std::string::npos) {
                        key = key.substr(key_start, key_end - key_start + 1);
                    }

                    pos = eq_pos + 1;
                    while (pos < args_part.length() &&
                        (args_part[pos] == ' ' || args_part[pos] == '\t')) {
                        pos++;
                    }

                    std::string value;
                    if (pos < args_part.length() && args_part[pos] == '"') {
                        pos++;
                        size_t value_end = pos;
                        while (value_end < args_part.length() && args_part[value_end] != '"') {
                            if (args_part[value_end] == '\\') value_end++;
                            value_end++;
                        }
                        value = args_part.substr(pos, value_end - pos);
                        pos = value_end + 1;
                        args_json[key] = value;
                    }
                    else {
                        size_t value_end = pos;
                        while (value_end < args_part.length() &&
                            args_part[value_end] != ',' && args_part[value_end] != ')') {
                            value_end++;
                        }
                        value = args_part.substr(pos, value_end - pos);
                        size_t v_start = value.find_first_not_of(" \t\n\r");
                        size_t v_end = value.find_last_not_of(" \t\n\r");
                        if (v_start != std::string::npos) {
                            value = value.substr(v_start, v_end - v_start + 1);
                        }
                        args_json[key] = value;
                        pos = value_end;
                    }
                }

                size_t next_tool_start = pos_close + 1;
                while (next_tool_start < raw.length() &&
                    (raw[next_tool_start] == ',' || raw[next_tool_start] == ' ' ||
                        raw[next_tool_start] == '\t' || raw[next_tool_start] == '\n' ||
                        raw[next_tool_start] == ']')) {
                    next_tool_start++;
                }

                if (next_tool_start < raw.length()) {
                    buffer_ = TOOL_START + "[" + raw.substr(next_tool_start) + "]" + TOOL_END + buffer_;
                }

                result.type = StreamEventType::TOOL_DONE;
                result.tool_id = "call_" + std::to_string(std::time(nullptr));
                result.tool_name = fn_name;
                result.tool_args_str = args_json.dump();

                return result;

            }
            else {
                tool_name_ += buffer_;
                buffer_.clear();
                result.type = StreamEventType::WAITING;
                return result;
            }
        }
    }

    result.type = StreamEventType::WAITING;
    return result;
}


/***********              LFM2_5_TK family            ***********/
LFM2_5_TK::LFM2_5_TK(xrt::device* npu_device_inst) : AutoModel(npu_device_inst, "LFM2_5_TK") {}

void LFM2_5_TK::load_model(std::string model_path, json model_info, int default_context_length, bool enable_preemption) {
    this->_shared_load_model(model_path, model_info, default_context_length, enable_preemption);

    this->q4nx = std::make_unique<Q4NX>(this->model_path);
    // model_type == llama
    this->lm_engine = std::make_unique<lfm2_npu>(*this->lm_config, this->npu.get(), this->MAX_L);

    this->lm_engine->load_weights(*this->q4nx);

    //free the q4nx
    this->q4nx.reset();
    this->lm_engine->clear_context();
    this->setup_tokenizer(model_path);
    this->sampler.reset();

    sampler_config config;
    config.rep_penalty = 1.05;
    config.temperature = 0.1;
    config.top_p = 0.1;
    config.top_k = 10;
    config.rep_penalty_window = 1024;
    config.freq_penalty = 1.0;
    config.freq_penalty_window = 1024;
    this->set_sampler(config);
    for (size_t i = 0; i < PROFILER_TYPE_NUM; i++) {
        this->profiler_list[i].reset();
    }
}

void LFM2_5_TK::setup_tokenizer(std::string model_path) {
    auto tokenizer_config = this->_shared_setup_tokenizer(model_path);
    this->tokenizer->is_doubled_encoded = true;
}

std::string LFM2_5_TK::apply_chat_template(nlohmann::ordered_json& messages, nlohmann::ordered_json tools) {
    minja::chat_template_inputs inputs;
    minja::chat_template_options opt;
    opt.polyfill_tool_responses = false;
    inputs.add_generation_prompt = true;
    inputs.messages = messages;
    inputs.extra_context = this->extra_context;
    // inputs.tools = tools;
    return this->chat_tmpl->apply(inputs, opt);
}

bool LFM2_5_TK::insert(chat_meta_info_t& meta_info, lm_uniform_input_t& input, std::function<bool()> is_cancelled) {
    // preprocess
    this->profiler_list[TKOEN_ENCODE_TIME].start();
    std::string templated_text;
    if (input.messages.empty() && input.prompt.empty()) {
        header_print("WARNING", "No messages or prompt provided");
        return false;
    }
    if (!input.messages.empty()) { // already a formated messages, usually from REST API
        templated_text = this->apply_chat_template(input.messages);
        // templated_text = this->apply_chat_template(input.messages, input.tools);
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
    lfm2_npu *lfm2_engine = dynamic_cast<lfm2_npu*>(this->lm_engine.get());

    if (meta_info.restore_allowed) {
        restore_idx = lfm2_engine->restore();
        this->total_tokens = restore_idx;
        this->token_history = checkpoint_his; // restore the token history to be consistent with the restored KV cache, which is crucial for correct functioning of _shared_insert's prefix-matching logic
    }

    // Remove the starting reasoning placeholder appended by the chat template: "<think>\n" -> token ids 151667 708.
    size_t n = tokens.size();
    tokens.resize(n - 2);

    bool success = this->_shared_insert(meta_info, tokens, is_cancelled, nullptr);

    checkpoint_his = token_history;
    int checkpoint_idx = lfm2_engine->checkpoint();

    return success;
}


std::string LFM2_5_TK::generate(chat_meta_info_t& meta_info, int length_limit, std::ostream& os, std::function<bool()> is_cancelled) {
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
    this->profiler_list[DECODING_TIME].reset();
    this->profiler_list[TKOEN_DECODE_TIME].reset();

    std::string token_str;
    int sampled_token;

    this->token_history.push_back(think_start_id);
    this->profiler_list[DECODING_TIME].start();
    this->lm_engine->forward(think_start_id);
    this->profiler_list[DECODING_TIME].stop(1);
    token_str = this->tokenizer->run_time_decoder(think_start_id);
    result += token_str;
    os << token_str << std::flush;

    // \n
    this->token_history.push_back(708);
    this->profiler_list[DECODING_TIME].start();
    buffer<bf16> y = this->lm_engine->forward(708);
    this->profiler_list[DECODING_TIME].stop(1);
    token_str = this->tokenizer->run_time_decoder(708);
    result += token_str;
    sampled_token = this->sampler->sample(y);
    os << token_str << std::flush;

    this->total_tokens++;
    meta_info.generated_tokens++;
    int last_sampled_token = sampled_token;
    token_str = this->tokenizer->run_time_decoder(last_sampled_token);
    result += token_str;
    os << token_str << std::flush;

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
    return result;
}

std::string LFM2_5_TK::generate_with_prompt(chat_meta_info_t& meta_info, lm_uniform_input_t& input, int length_limit, std::ostream& os) {
    if (!this->insert(meta_info, input)) {
        return "";
    }
    return this->generate(meta_info, length_limit, os);
}

NonStreamResult LFM2_5_TK::parse_nstream_content(const std::string response_text) {
    NonStreamResult result;

    std::string content, reasoning_content;

    std::string think_start_tag = "<think>";
    std::string think_end_tag = "</think>";

    size_t think_start_pos = response_text.find(think_start_tag);
    size_t think_end_pos = response_text.find(think_end_tag);


    think_start_pos += think_start_tag.length();
    std::string reasoning_str = response_text.substr(think_start_pos, think_end_pos - think_start_pos);
    result.reasoning_content = reasoning_str;

    std::string content_str = response_text.substr(think_end_pos + think_end_tag.length());
    result.content = content_str;

    return result;
}

StreamResult LFM2_5_TK::parse_stream_content(const std::string content) {
    const std::string TOOL_START = "<|tool_call_start|>";
    const std::string TOOL_END = "<|tool_call_end|>";
    const std::string MARKER_THINK_START = "<think>";
    const std::string MARKER_THINK_END = "</think>";

    StreamResult result;
    buffer_ += content;

    while (true) {
        if (!is_in_tool_block_) {
            size_t tool_start_pos = buffer_.find(TOOL_START);

            if (tool_start_pos != std::string::npos) {
                if (tool_start_pos > 0) {
                    result.content = buffer_.substr(0, tool_start_pos);
                    result.type = current_mode_;
                    buffer_ = buffer_.substr(tool_start_pos);
                    return result;
                }

                is_in_tool_block_ = true;
                tool_name_.clear();
                buffer_ = buffer_.substr(TOOL_START.length());
                result.type = StreamEventType::WAITING;
                return result;
            }
            //else {
            //    if (!buffer_.empty()) {
            //        result.content = buffer_;
            //        result.type = StreamEventType::CONTENT;
            //        buffer_.clear();
            //        return result;
            //    }
            //    break;
            //}
        }

        if (is_in_tool_block_) {
            size_t tool_end_pos = buffer_.find(TOOL_END);

            if (tool_end_pos != std::string::npos) {
                tool_name_ += buffer_.substr(0, tool_end_pos);
                buffer_ = buffer_.substr(tool_end_pos + TOOL_END.length());
                is_in_tool_block_ = false;

                std::string raw = tool_name_;

                size_t start = raw.find_first_not_of(" \t\n\r[");
                if (start == std::string::npos) {
                    result.type = StreamEventType::CONTENT;
                    result.content = "[Error: Empty tool call]";
                    return result;
                }
                raw = raw.substr(start);

                size_t pos_open = raw.find('(');
                if (pos_open == std::string::npos) {
                    result.type = StreamEventType::CONTENT;
                    result.content = "[Error: No function bracket found]";
                    return result;
                }

                std::string fn_name = raw.substr(0, pos_open);
                size_t name_end = fn_name.find_last_not_of(" \t\n\r");
                fn_name = fn_name.substr(0, name_end + 1);

                int paren_count = 0;
                size_t pos_close = pos_open;
                for (size_t i = pos_open; i < raw.length(); ++i) {
                    if (raw[i] == '(') paren_count++;
                    if (raw[i] == ')') {
                        paren_count--;
                        if (paren_count == 0) {
                            pos_close = i;
                            break;
                        }
                    }
                }

                std::string args_part = raw.substr(pos_open + 1, pos_close - pos_open - 1);

                json args_json = json::object();

                size_t pos = 0;
                while (pos < args_part.length()) {
                    while (pos < args_part.length() &&
                        (args_part[pos] == ' ' || args_part[pos] == ',' ||
                            args_part[pos] == '\t' || args_part[pos] == '\n')) {
                        pos++;
                    }
                    if (pos >= args_part.length()) break;

                    size_t eq_pos = args_part.find('=', pos);
                    if (eq_pos == std::string::npos) break;

                    std::string key = args_part.substr(pos, eq_pos - pos);
                    size_t key_start = key.find_first_not_of(" \t\n\r");
                    size_t key_end = key.find_last_not_of(" \t\n\r");
                    if (key_start != std::string::npos) {
                        key = key.substr(key_start, key_end - key_start + 1);
                    }

                    pos = eq_pos + 1;
                    while (pos < args_part.length() &&
                        (args_part[pos] == ' ' || args_part[pos] == '\t')) {
                        pos++;
                    }

                    std::string value;
                    if (pos < args_part.length() && args_part[pos] == '"') {
                        pos++;
                        size_t value_end = pos;
                        while (value_end < args_part.length() && args_part[value_end] != '"') {
                            if (args_part[value_end] == '\\') value_end++;
                            value_end++;
                        }
                        value = args_part.substr(pos, value_end - pos);
                        pos = value_end + 1;
                        args_json[key] = value;
                    }
                    else {
                        size_t value_end = pos;
                        while (value_end < args_part.length() &&
                            args_part[value_end] != ',' && args_part[value_end] != ')') {
                            value_end++;
                        }
                        value = args_part.substr(pos, value_end - pos);
                        size_t v_start = value.find_first_not_of(" \t\n\r");
                        size_t v_end = value.find_last_not_of(" \t\n\r");
                        if (v_start != std::string::npos) {
                            value = value.substr(v_start, v_end - v_start + 1);
                        }
                        args_json[key] = value;
                        pos = value_end;
                    }
                }

                size_t next_tool_start = pos_close + 1;
                while (next_tool_start < raw.length() &&
                    (raw[next_tool_start] == ',' || raw[next_tool_start] == ' ' ||
                        raw[next_tool_start] == '\t' || raw[next_tool_start] == '\n' ||
                        raw[next_tool_start] == ']')) {
                    next_tool_start++;
                }

                if (next_tool_start < raw.length()) {
                    buffer_ = TOOL_START + "[" + raw.substr(next_tool_start) + "]" + TOOL_END + buffer_;
                }

                result.type = StreamEventType::TOOL_DONE;
                result.tool_id = "call_" + std::to_string(std::time(nullptr));
                result.tool_name = fn_name;
                result.tool_args_str = args_json.dump();

                return result;

            }
            else {
                tool_name_ += buffer_;
                buffer_.clear();
                result.type = StreamEventType::WAITING;
                return result;
            }
        }

        // normal and reasoning
        if (current_mode_ == StreamEventType::CONTENT) {
            size_t think_start_pos = buffer_.find(MARKER_THINK_START);

            if (think_start_pos != std::string::npos) {
                if (think_start_pos > 0) {
                    result.content = buffer_.substr(0, think_start_pos);
                    result.type = StreamEventType::CONTENT;
                    buffer_ = buffer_.substr(think_start_pos);
                    return result;
                }
                buffer_ = buffer_.substr(MARKER_THINK_START.length());
                current_mode_ = StreamEventType::REASONING;
                continue;
            }
        }
        else if (current_mode_ == StreamEventType::REASONING) {
            size_t think_end_pos = buffer_.find(MARKER_THINK_END);

            if (think_end_pos != std::string::npos) {
                if (think_end_pos > 0) {
                    result.content = buffer_.substr(0, think_end_pos);
                    result.type = StreamEventType::REASONING;
                    buffer_ = buffer_.substr(think_end_pos);
                    return result;
                }
                buffer_ = buffer_.substr(MARKER_THINK_END.length());
                current_mode_ = StreamEventType::CONTENT;
                continue;
            }
        }

        if (!buffer_.empty()) {
            result.content = buffer_;
            result.type = current_mode_;
            buffer_.clear();
            return result;
        }

        break;

    }

    result.type = current_mode_;
    return result;
}
