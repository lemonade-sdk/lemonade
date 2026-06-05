/// \file nanbeige.cpp
/// \brief nanbeige class
/// \author FastFlowLM Team
/// \date 2025-09-04
/// \version 0.9.24
/// \note This is a source file for the nanbeige class

#include "AutoModel/modeling_nanbeige.hpp"

/************              Nanbeige family            **************/
Nanbeige::Nanbeige(xrt::device* npu_device_inst) : AutoModel(npu_device_inst, "Nanbeige") {}

void Nanbeige::load_model(std::string model_path, json model_info, int default_context_length, bool enable_preemption) {
    this->_shared_load_model(model_path, model_info, default_context_length, enable_preemption);

    this->q4nx = std::make_unique<Q4NX>(this->model_path);
    // model_type == nanbeige
    this->lm_engine = std::make_unique<nanbeige_npu>(*this->lm_config, this->npu.get(), this->MAX_L);

    this->lm_engine->load_weights(*this->q4nx);

    //free the q4nx
    this->q4nx.reset();

    this->lm_engine->clear_context();
    this->setup_tokenizer(model_path);
    this->sampler.reset();

    sampler_config config;
    config.top_k = 20;
    config.top_p = 0.95;
    config.min_p = 0.0;
    config.temperature = 0.6;

    this->set_sampler(config);
    for (size_t i = 0; i < PROFILER_TYPE_NUM; i++) {
        this->profiler_list[i].reset();
    }
}

void Nanbeige::setup_tokenizer(std::string model_path) {
    auto tokenizer_config = this->_shared_setup_tokenizer(model_path);
}

std::string Nanbeige::apply_chat_template(nlohmann::ordered_json& messages, nlohmann::ordered_json tools) {
    minja::chat_template_inputs inputs;
    inputs.add_generation_prompt = true;
    inputs.messages = messages;
    // inputs.tools = tools;
    inputs.extra_context = this->extra_context;
    return this->chat_tmpl->apply(inputs);
}

std::string Nanbeige::nanbeige_filter(int token) {

    std::string token_str = this->tokenizer->run_time_decoder(token);
    // Nanbeige tokenizer encodes hex-unicode form 0x00-0xFF as <0xXX> from 3 to 258
    if (token >= 3 && token <= 258){
        char c;
        sscanf(token_str.c_str(), "<0x%hhx>", &c);
        token_str.resize(1);
        token_str[0] = c;
    }
    return token_str;
}

bool Nanbeige::insert(chat_meta_info_t& meta_info, lm_uniform_input_t& input, std::function<bool()> is_cancelled) {
    // preprocess
    this->profiler_list[TKOEN_ENCODE_TIME].start();
    std::string templated_text;
    if (input.messages.empty() && input.prompt.empty()) {
        header_print("WARNING", "No messages or prompt provided");
        return false;
    }
    if (!input.messages.empty()) { // already a formated messages, usually from REST API
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
    int restore_idx = -1;
    nanbeige_npu *nanbeige_engine = dynamic_cast<nanbeige_npu*>(this->lm_engine.get());

    if (meta_info.restore_allowed) {
        restore_idx = nanbeige_engine->restore();
        this->total_tokens = restore_idx;
        this->token_history = checkpoint_his; // restore the token history to be consistent with the restored KV cache, which is crucial for correct functioning of _shared_insert's prefix-matching logic
    }

    bool success = this->_shared_insert(meta_info, tokens, is_cancelled, nullptr);

    checkpoint_his = token_history;
    int checkpoint_idx = nanbeige_engine->checkpoint();

    return success;
}


std::string Nanbeige::generate(chat_meta_info_t& meta_info, int length_limit, std::ostream& os, std::function<bool()> is_cancelled) {
    //header_print("is_cancelled", is_cancelled);
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
    this->token_history.push_back(this->last_token);
    if (this->is_normal_token(last_sampled_token) && last_sampled_token != -1){
        std::string token_str = this->nanbeige_filter(last_sampled_token);
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
            std::string token_str = this->nanbeige_filter(sampled_token);
            os << token_str << std::flush;
            result += token_str;
        }
        this->profiler_list[TKOEN_DECODE_TIME].stop(1);
        this->token_history.push_back(sampled_token);
        if (this->is_eos(sampled_token)){
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

std::string Nanbeige::generate_with_prompt(chat_meta_info_t& meta_info, lm_uniform_input_t& input, int length_limit, std::ostream& os) {
    if (!this->insert(meta_info, input)) {
        return "";
    }
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
    this->token_history.push_back(this->last_token);
    if (this->is_normal_token(last_sampled_token) && last_sampled_token != -1){
        std::string token_str = this->nanbeige_filter(last_sampled_token);
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
            std::string token_str = this->nanbeige_filter(sampled_token);
            os << token_str << std::flush;
            result += token_str;
        }
        this->profiler_list[TKOEN_DECODE_TIME].stop(1);
        this->token_history.push_back(sampled_token);
        if (this->is_eos(sampled_token)){
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
    return result;
}

NonStreamResult Nanbeige::parse_nstream_content(const std::string response_text) {
    NonStreamResult result;

    std::string name, arguments;
    std::string content, reasoning_content;

    std::string think_start_tag = "<think>";
    std::string think_end_tag = "</think>";
    std::string tool_start_tag = "<tool_call>";
    std::string tool_end_tag = "</tool_call>";

    size_t think_start_pos = response_text.find(think_start_tag);
    size_t think_end_pos = response_text.find(think_end_tag);
    size_t tool_start_pos = response_text.find(tool_start_tag);
    size_t tool_end_pos = response_text.find(tool_end_tag);

    bool is_reasoning = !(think_start_pos == std::string::npos || think_end_pos == std::string::npos);
    bool is_tool = !(tool_start_pos == std::string::npos || tool_end_pos == std::string::npos);
    bool is_content = !is_tool;

    if (is_reasoning) {
        // Find reasoning part
        think_start_pos += think_start_tag.length();
        std::string reasoning_str = response_text.substr(think_start_pos, think_end_pos - think_start_pos);
        result.reasoning_content = reasoning_str;
    }

    if (is_tool) {
        // Find tool calling part
        tool_start_pos += tool_start_tag.length();
        std::string json_str = response_text.substr(tool_start_pos, tool_end_pos - tool_start_pos);
        // Parse "name"
        std::string key_name = "\"name\": \"";
        size_t name_start = json_str.find(key_name);
        if (name_start != std::string::npos) {
            name_start += key_name.length();
            size_t name_end = json_str.find("\"", name_start);
            if (name_end != std::string::npos) {
                name = json_str.substr(name_start, name_end - name_start);
            }
        }
        // Parse "arguments"
        std::string key_args = "\"arguments\":";
        size_t args_pos = json_str.find(key_args);
        if (args_pos != std::string::npos) {
            size_t brace_start = json_str.find("{", args_pos);
            size_t brace_end = json_str.rfind("}"); // Find the last closing brace

            if (brace_start != std::string::npos && brace_end != std::string::npos && brace_end > brace_start) {
                arguments = json_str.substr(brace_start, brace_end - brace_start);
            }
        }

        result.tool_name = name;
        result.tool_args = arguments;

    }
    else if (is_content) {
        std::string content_str = response_text.substr(think_end_pos + think_end_tag.length());
        result.content = content_str;
    }

    return result;
}


StreamResult Nanbeige::parse_stream_content(const std::string content) {
    const std::string MARKER_THINK_START = "<think>";
    const std::string MARKER_THINK_END = "</think>";
    const std::string MARKER_TOOL_START = "<tool_call>";
    const std::string MARKER_TOOL_END = "</tool_call>";

    StreamResult result;
    buffer_ += content;

    while (true) {
        // Keep data in buffer_ and wait until a complete TOOL_END is found. Never clear buffer_ midway.
        if (is_in_tool_block_) {
            size_t tool_end_pos = buffer_.find(MARKER_TOOL_END);
            if (tool_end_pos != std::string::npos) {
                std::string tool_content = buffer_.substr(0, tool_end_pos);
                buffer_ = buffer_.substr(tool_end_pos + MARKER_TOOL_END.length());
                is_in_tool_block_ = false;

                try {
                    auto j = nlohmann::json::parse(tool_content);
                    result.type = StreamEventType::TOOL_DONE;
                    result.tool_id = "generate_id()";

                    if (j.contains("name")) {
                        result.tool_name = j["name"].get<std::string>();
                    }
                    if (j.contains("arguments")) {
                        if (j["arguments"].is_string()) {
                            result.tool_args_str = j["arguments"].get<std::string>();
                        } else {
                            result.tool_args_str = j["arguments"].dump();
                        }
                    }
                    return result;
                } catch (...) {
                    result.type = StreamEventType::CONTENT;
                    result.content = "[Error parsing tool call]";
                    return result;
                }
            } else {
                result.type = StreamEventType::WAITING;
                return result;
            }
        }

        // tool start
        size_t tool_start_pos = buffer_.find(MARKER_TOOL_START);
        if (tool_start_pos != std::string::npos) {
            if (tool_start_pos > 0) {
                result.content = buffer_.substr(0, tool_start_pos);
                result.type = current_mode_;
                buffer_ = buffer_.substr(tool_start_pos);
                return result;
            }
            is_in_tool_block_ = true;
            buffer_ = buffer_.substr(MARKER_TOOL_START.length());
            continue;
        }

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
        } else if (current_mode_ == StreamEventType::REASONING) {
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

        // 4. Safe Flush mechanism: Handle fragmented tag headers
        // E.g., if the current chunk ends with "<too", keep "<too" in the buffer to prevent truncation
        std::vector<std::string> active_markers;
        if (current_mode_ == StreamEventType::CONTENT) {
            active_markers.push_back(MARKER_THINK_START);
            active_markers.push_back(MARKER_TOOL_START);
        } else if (current_mode_ == StreamEventType::REASONING) {
            active_markers.push_back(MARKER_THINK_END);
            active_markers.push_back(MARKER_TOOL_START);
        }

        size_t safe_flush_len = buffer_.length();
        for (const auto& marker : active_markers) {
            for (size_t i = 1; i <= marker.length() && i <= buffer_.length(); ++i) {
                if (buffer_.compare(buffer_.length() - i, i, marker, 0, i) == 0) {
                    safe_flush_len = std::min(safe_flush_len, buffer_.length() - i);
                }
            }
        }

        if (safe_flush_len > 0) {
            result.content = buffer_.substr(0, safe_flush_len);
            result.type = current_mode_;
            buffer_ = buffer_.substr(safe_flush_len);
            return result;
        } else if (buffer_.length() > 0) {
            result.type = StreamEventType::WAITING;
            return result;
        }

        break;
    }

    result.type = current_mode_;
    return result;
}
