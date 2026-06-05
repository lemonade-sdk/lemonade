/// \file deepseek.cpp
/// \brief deepseek class
/// \author FastFlowLM Team
/// \date 2025-09-01
/// \version 0.9.24
/// \note This is a source file for the deepseek class


#include "AutoModel/modeling_qwen3vl.hpp"
#include "metrices.hpp"


/************              Qwen3VL family            **************/
Qwen3VL::Qwen3VL(xrt::device* npu_device_inst) : AutoModel(npu_device_inst, "Qwen3VL") {}

void Qwen3VL::load_model(std::string model_path, json model_info, int default_context_length, bool enable_preemption) {
    this->_shared_load_model(model_path, model_info, default_context_length, enable_preemption);

    this->q4nx = std::make_unique<Q4NX>(this->model_path);
    // lm_config->model_type == qwen3
    this->lm_engine = std::make_unique<qwen3vl_npu>(*this->lm_config, this->npu.get(), this->MAX_L);

    this->lm_engine->load_weights(*this->q4nx);
    //free the q4nx
    this->q4nx.reset();
    this->lm_engine->clear_context();
    this->setup_tokenizer(model_path);
    this->sampler.reset();

    sampler_config config;
    config.top_k = 40;
    config.top_p = 0.9;
    config.min_p = 0.1;
    config.temperature = 0.8;
    config.rep_penalty = 1.05;
    config.freq_penalty = 1.05;

    this->set_sampler(config);
    for (size_t i = 0; i < PROFILER_TYPE_NUM; i++) {
        this->profiler_list[i].reset();
    }
}

void Qwen3VL::setup_tokenizer(std::string model_path) {
    auto tokenizer_config = this->_shared_setup_tokenizer(model_path);
}

std::string Qwen3VL::apply_chat_template(nlohmann::ordered_json& messages, nlohmann::ordered_json tools) {
    minja::chat_template_inputs inputs;
    inputs.add_generation_prompt = true;
    inputs.messages = messages;

    if (!tools.empty())
        inputs.tools = tools;
    inputs.extra_context = this->extra_context;
    return this->chat_tmpl->apply(inputs);
}

bool Qwen3VL::insert(chat_meta_info_t& meta_info, lm_uniform_input_t& input, std::function<bool()> is_cancelled) {
    // preprocess
    constexpr int image_soft_token_id = 151655;
    this->profiler_list[TKOEN_ENCODE_TIME].start();
    std::string templated_text;
    if (input.messages.empty() && input.prompt.empty()) {
        header_print("WARNING", "No messages or prompt provided");
        return false;
    }

    constexpr bool DEBUG_IMAGE_PREPROCESS = false;
    qwen3vl_image_payload_t image_payload;
    image_payload.num_images = 0;
    if (input.images.size() > 0) {


        // header_print("info", "Processing images...");

        // time_utils::time_point preprocess_start = time_utils::now();
        for(const auto& img_str : input.images){
            qwen3vl_image_t image = this->load_image(img_str);

            preprocess_image(image, image_payload._data__processed);
            // Push the image AFTER preprocessing so grid_h and grid_w are set
            image_payload.images.push_back(image);
            image_payload.num_images++;
        }
    }
    if (!input.messages.empty()) { // already a formated messages, usually from REST API
        json qwenvl_message = json::array();
        for (const auto& item : input.messages) {
            if (!item.contains("images")) {
                qwenvl_message.push_back(item);
                continue;
            }

            json newContent = json::array();
            for (const auto& img : item["images"]) {
                newContent.push_back({
                    {"type", "image"},
                    {"image", img}
                });
            }
            newContent.push_back({
                {"type", "text"},
                {"text", item["content"]}
            });

            json newItem = {
                {"role", item["role"]},
                {"content", newContent}
            };

            qwenvl_message.push_back(newItem);
        }
        templated_text = this->apply_chat_template(qwenvl_message, input.tools);
        int total_images = 0;
        for (auto& message : qwenvl_message) {
            auto content = message.value("content", nlohmann::ordered_json::array());
            for (auto& item : content) {
                if (item.contains("type") && item["type"] == "image") {
                    std::string img_str = item.value("image", "");
                    if (!img_str.empty()) {
                        total_images++;
                    }
                    qwen3vl_image_t image = this->load_image_base64(img_str);
                    preprocess_image(image, image_payload._data__processed);
                    image_payload.images.push_back(image);
                    image_payload.num_images++;
                }
            }
        }
        header_print("FLM", "Total images: " << total_images);
    }
    else if (!input.prompt.empty()) { // a pure text, usually from the cli
        nlohmann::ordered_json messages;
        nlohmann::ordered_json content;
        content["role"] = "user";
        content["content"] = nlohmann::ordered_json::array();

        // Add image objects to content array
        for (int i = 0; i < input.images.size(); i++) {
            nlohmann::ordered_json image_obj;
            image_obj["type"] = "image";
            image_obj["image"] = input.images[i];
            content["content"].push_back(image_obj);
        }

        // Add text object to content array
        nlohmann::ordered_json text_obj;
        text_obj["type"] = "text";
        text_obj["text"] = input.prompt;
        content["content"].push_back(text_obj);

        messages.push_back(content);
        templated_text = this->apply_chat_template(messages);
    }
    std::vector<int> tokens_init = this->tokenizer->encode(templated_text);

    // update the tokens to include the image tokens
    std::vector<int> tokens;
    int total_image_tokens = 0;
    // Use image_payload.images.size() (not input.images.size()), because on
    // the REST API path images come from `messages` and input.images is empty.
    for (size_t i = 0; i < image_payload.images.size(); i++) {
        total_image_tokens += image_payload.images[i].grid_h * image_payload.images[i].grid_w;
    }
    tokens.reserve(tokens_init.size() + total_image_tokens);
    int image_counter = 0;
    for (int i = 0; i < tokens_init.size(); i++) {
        if (tokens_init[i] == image_soft_token_id) {
            for (int j = 0; j < image_payload.images[image_counter].grid_h * image_payload.images[image_counter].grid_w / 4; j++) {
                tokens.push_back(image_soft_token_id);
            }
            image_counter++;
        } else {
            tokens.push_back(tokens_init[i]);
        }
    }

    this->profiler_list[TKOEN_ENCODE_TIME].stop(tokens.size());

    // ----------------------------------------------------------------------
    // Prompt-cache aware image alignment.
    //
    // AutoModel::_shared_insert prefix-matches `tokens` against `token_history`
    // over the FULL length of `token_history`. If every token matches, it
    // erases that prefix before prefilling; otherwise it calls clear_context()
    // and skips nothing. We must NOT erase `tokens` here -- _shared_insert
    // needs the untrimmed sequence to run that very check. What we DO need to
    // fix up locally is the image payload (pixels for the WHOLE prompt,
    // including already-cached images from earlier turns): drop the
    // fully-cached leading images so the surviving payload aligns with the
    // surviving image tokens after _shared_insert performs its own erase.
    // ----------------------------------------------------------------------
    size_t prefix_skip_count = 0;
    {
        const size_t idx = this->token_history.size();
        for (size_t i = 0; i < idx; i++) {
            if (i < tokens.size() && tokens[i] == this->token_history[i]) {
                prefix_skip_count++;
            } else {
                break;
            }
        }
        // Must match the entirety of token_history, otherwise _shared_insert
        // will clear the context and not skip anything.
        if (prefix_skip_count != idx) {
            prefix_skip_count = 0;
        }

        if (prefix_skip_count > 0 && !image_payload.images.empty()) {
            // Count image-soft tokens in the cached prefix.
            int skipped_image_tokens = 0;
            for (size_t i = 0; i < prefix_skip_count; i++) {
                if (tokens[i] == image_soft_token_id) skipped_image_tokens++;
            }

            // Walk through images and drop those whose entire token block
            // sits within the cached prefix. (The chat-template prefix
            // boundary always falls between messages, so an image's token
            // block is never partially cached.)
            size_t images_to_drop = 0;
            size_t bf16_to_drop = 0;
            int consumed_image_tokens = 0;
            for (const auto& img : image_payload.images) {
                const int img_tokens =
                    (img.grid_h * img.grid_w) / 4;
                const size_t img_bf16 =
                    static_cast<size_t>(img.grid_h) * QWEN3_PATCH_SIZE *
                    static_cast<size_t>(img.grid_w) * QWEN3_PATCH_SIZE *
                    3u * QWEN3_TEMPORAL_PATCH_SIZE;
                if (consumed_image_tokens + img_tokens <= skipped_image_tokens) {
                    consumed_image_tokens += img_tokens;
                    bf16_to_drop += img_bf16;
                    images_to_drop++;
                } else {
                    break;
                }
            }

            if (images_to_drop > 0) {
                image_payload.images.erase(
                    image_payload.images.begin(),
                    image_payload.images.begin() + images_to_drop);
                image_payload.num_images -= static_cast<int>(images_to_drop);
                if (bf16_to_drop >= image_payload._data__processed.size()) {
                    image_payload._data__processed.clear();
                } else {
                    image_payload._data__processed.erase(
                        image_payload._data__processed.begin(),
                        image_payload._data__processed.begin() + bf16_to_drop);
                }
                header_print("FLM",
                    "Prompt-cache hit: dropped " << images_to_drop
                    << " cached image(s) from payload");
            }
        }
    }

    // find the last image token index, expressed relative to the tokens that
    // will SURVIVE _shared_insert's prefix erase (i.e. shifted by -prefix_skip_count).
    int last_image_token_index = -1;
    for (int i = static_cast<int>(prefix_skip_count); i < (int)tokens.size(); i++) {
        if (tokens[i] == image_soft_token_id) {
            last_image_token_index = i - static_cast<int>(prefix_skip_count);
        }
    }
    last_image_token_index++; // plus the end of image tokens

    // hardware
    if (image_payload.num_images > 0){
        return this->_shared_insert(meta_info, tokens, is_cancelled, &image_payload, last_image_token_index);
    }else{
        return this->_shared_insert(meta_info, tokens, is_cancelled, nullptr);
    }

}

std::string Qwen3VL::generate(chat_meta_info_t& meta_info, int length_limit, std::ostream& os, std::function<bool()> is_cancelled) {
    return this->_shared_generate(meta_info, length_limit, os, is_cancelled);
}

std::string Qwen3VL::generate_with_prompt(chat_meta_info_t& meta_info, lm_uniform_input_t& input, int length_limit, std::ostream& os) {
    if (!this->insert(meta_info, input)) {
        return "";
    }
    return this->_shared_generate(meta_info, length_limit, os);
}

// Non-stream
NonStreamResult Qwen3VL::parse_nstream_content(const std::string response_text) {
    NonStreamResult result;

    std::string name, arguments;

    std::string start_tag = "<tool_call>";
    std::string end_tag = "</tool_call>";

    size_t start_pos = response_text.find(start_tag);
    size_t end_pos = response_text.find(end_tag);

    if (start_pos == std::string::npos || end_pos == std::string::npos) {
        // pure content
        result.content = response_text;
        return result;
    }

    start_pos += start_tag.length();
    std::string json_str = response_text.substr(start_pos, end_pos - start_pos);

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

    return result;
}

// Stream
StreamResult Qwen3VL::parse_stream_content(const std::string content) {
    std::string tool_start_tag = "<tool_call>";
    std::string tool_end_tag = "</tool_call>";

    StreamResult result;
    result.type = StreamEventType::CONTENT;

    if (content.find(tool_start_tag) != std::string::npos) {
        is_in_tool_block_ = true;
        tool_name_.clear();
        result.type = StreamEventType::WAITING;
        return result;
    }

    if (content.find("</tool_call>") != std::string::npos) {
        is_in_tool_block_ = false;

        try {
            auto j = nlohmann::json::parse(tool_name_);

            result.type = StreamEventType::TOOL_DONE;
            //result.tool_id = generate_id();
            result.tool_id = "call_" + std::to_string(std::time(nullptr));

            if (j.contains("name")) {
                result.tool_name = j["name"].get<std::string>();
            }

            if (j.contains("arguments")) {
                if (j["arguments"].is_string()) {
                    result.tool_args_str = j["arguments"].get<std::string>();
                }
                else {
                    result.tool_args_str = j["arguments"].dump();
                }
            }
        }
        catch (...) {
            result.type = StreamEventType::CONTENT;
            result.content = "[Error parsing tool call]";
        }
        return result;
    }

    if (is_in_tool_block_) {
        tool_name_ += content;
        result.type = StreamEventType::WAITING;
        return result;
    }

    result.content = content;
    return result;

}


/************              Qwen3VL_Thinking            **************/

std::string Qwen3VL_Thinking::generate_with_prompt(chat_meta_info_t& meta_info, lm_uniform_input_t& input, int length_limit, std::ostream& os) {
    if (!this->insert(meta_info, input)) {
        return "";
    }
    return this->generate(meta_info, length_limit, os);
}


std::string Qwen3VL_Thinking::generate(chat_meta_info_t& meta_info, int length_limit, std::ostream& os, std::function<bool()> is_cancelled) {
    std::string result;
    os << "<think>\n\n";
    result = this->_shared_generate(meta_info, length_limit, os, is_cancelled);
    result = "<think>\n\n" + result;
    return result;
}
