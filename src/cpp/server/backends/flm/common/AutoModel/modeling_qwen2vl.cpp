/// \file deepseek.cpp
/// \brief deepseek class
/// \author FastFlowLM Team
/// \date 2025-09-01
/// \version 0.9.24
/// \note This is a source file for the deepseek class


#include "AutoModel/modeling_qwen2vl.hpp"
#include "metrices.hpp"
#include <string_view>


/************              Qwen2VL family            **************/
Qwen2VL::Qwen2VL(xrt::device* npu_device_inst) : AutoModel(npu_device_inst, "Qwen2VL") {}

void Qwen2VL::load_model(std::string model_path, json model_info, int default_context_length, bool enable_preemption) {
    this->_shared_load_model(model_path, model_info, default_context_length, enable_preemption);

    this->q4nx = std::make_unique<Q4NX>(this->model_path);
    // lm_config->model_type == qwen2
    this->lm_engine = std::make_unique<qwen2vl_npu>(*this->lm_config, this->npu.get(), this->MAX_L);

    this->lm_engine->load_weights(*this->q4nx);
    //free the q4nx
    this->q4nx.reset();
    this->lm_engine->clear_context();
    this->setup_tokenizer(model_path);
    this->sampler.reset();

    sampler_config config;
    config.rep_penalty = 1.05;
    config.temperature = 0.6;
    config.top_p = 0.8;
    config.top_k = 10;
    config.rep_penalty_window = 1024;
    config.freq_penalty = 1.05;
    config.freq_penalty_window = 1024;
    this->set_sampler(config);
    for (size_t i = 0; i < PROFILER_TYPE_NUM; i++) {
        this->profiler_list[i].reset();
    }
}

void Qwen2VL::setup_tokenizer(std::string model_path) {
    auto tokenizer_config = this->_shared_setup_tokenizer(model_path);
}

std::string Qwen2VL::apply_chat_template(nlohmann::ordered_json& messages, nlohmann::ordered_json tools) {
    minja::chat_template_inputs inputs;
    inputs.add_generation_prompt = true;
    inputs.messages = messages;
    inputs.extra_context = this->extra_context;
    return this->chat_tmpl->apply(inputs);
}

bool Qwen2VL::insert(chat_meta_info_t& meta_info, lm_uniform_input_t& input, std::function<bool()> is_cancelled) {
    // preprocess
    constexpr int image_soft_token_id = 151655;
    this->profiler_list[TKOEN_ENCODE_TIME].start();
    std::string templated_text;
    if (input.messages.empty() && input.prompt.empty()) {
        header_print_r("WARNING", "No messages or prompt provided");
        return false;
    }

    constexpr bool DEBUG_IMAGE_PREPROCESS = false;
    qwen2vl_image_payload_t image_payload;
    image_payload.num_images = 0;
    if (input.images.size() > 0) {


        // header_print("info", "Processing images...");

        // time_utils::time_point preprocess_start = time_utils::now();
        for(const auto& img_str : input.images){
            qwen2vl_image_t image = this->load_image(img_str);

            preprocess_image(image, image_payload._data__processed);

            // Push the image AFTER preprocessing so grid_h and grid_w are set
            image_payload.images.push_back(image);
            image_payload.num_images++;
        }
    }
    try {
        if (!input.messages.empty()) { // already a formated messages, usually from REST API
            json qwenvl_message = json::array();
            std::vector<const std::string*> pending_images;
            int total_images = 0;
            for (const auto& item : input.messages) {
                if (!item.contains("images")) {
                    qwenvl_message.push_back(item);
                    continue;
                }

                json newContent = json::array();
                for (const auto& img : item["images"]) {
                    if (img.is_string()) {
                        pending_images.push_back(&img.get_ref<const std::string&>());
                        total_images++;
                    }
                    newContent.push_back({
                        {"type", "image"}
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
            templated_text = this->apply_chat_template(qwenvl_message);

            for (const std::string* img_ptr : pending_images) {
                if (img_ptr == nullptr || img_ptr->empty()) {
                    continue;
                }
                const std::string_view img_preview =
                    std::string_view(*img_ptr).substr(0, 8);
                // header_print_g("DEBUG", "Loading image: " + std::string(img_preview) + "...");
                qwen2vl_image_t image = this->load_image_base64(*img_ptr);
                // header_print_g("DEBUG", "Preprocessing image...");
                preprocess_image(image, image_payload._data__processed);
                // header_print_g("DEBUG", "Image preprocessed: " +
                    // std::to_string(image.width_resized) + "x" +
                    // std::to_string(image.height_resized) +
                    // ", grid: " + std::to_string(image.grid_w) + "x" +
                    // std::to_string(image.grid_h));
                // header_print_g("DEBUG", "Payload size: " + std::to_string(image_payload._data__processed.size()) );

                image_payload.images.push_back(image);
                image_payload.num_images++;
            }

            for (auto& message : qwenvl_message) {
                auto content = message.value("content", nlohmann::ordered_json::array());
                for (auto& item : content) {
                    if (item.contains("type") && item["type"] == "image") {
                        const std::string* img_ptr = nullptr;
                        if (item.contains("image") && item["image"].is_string()) {
                            img_ptr = &item["image"].get_ref<const std::string&>();
                        }
                        if (img_ptr == nullptr || img_ptr->empty()) {
                            continue;
                        }
                        const std::string_view img_preview =
                            std::string_view(*img_ptr).substr(0, 8);
                        // header_print_g("DEBUG", "Loading image: " + std::string(img_preview) + "...");
                        qwen2vl_image_t image = this->load_image_base64(*img_ptr);
                        // header_print_g("DEBUG", "Preprocessing image...");
                        preprocess_image(image, image_payload._data__processed);
                        // header_print_g("DEBUG", "Image preprocessed: " +
                            // std::to_string(image.width_resized) + "x" +
                            // std::to_string(image.height_resized) +
                            // ", grid: " + std::to_string(image.grid_w) + "x" +
                            // std::to_string(image.grid_h));
                        // header_print_g("DEBUG", "Payload size: " + std::to_string(image_payload._data__processed.size()) );

                        image_payload.images.push_back(image);
                        image_payload.num_images++;
                    }
                }
            }
            header_print_g("FLM", "Total images: " + std::to_string(total_images));
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
    } catch (const std::exception& e) {
        header_print_r("ERROR", std::string("Exception during chat template application: ") + e.what());
        return false;
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
                const int img_tokens = (img.grid_h * img.grid_w) / 4;
                const size_t img_bf16 =
                    static_cast<size_t>(img.grid_h) * QWEN2_PATCH_SIZE *
                    static_cast<size_t>(img.grid_w) * QWEN2_PATCH_SIZE *
                    3u * QWEN2_TEMPORAL_PATCH_SIZE;
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
                header_print_g("FLM",
                    "Prompt-cache hit: dropped " + std::to_string(images_to_drop)
                    + " cached image(s) from payload");
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

std::string Qwen2VL::generate(chat_meta_info_t& meta_info, int length_limit, std::ostream& os, std::function<bool()> is_cancelled) {
    return this->_shared_generate(meta_info, length_limit, os, is_cancelled);
}

std::string Qwen2VL::generate_with_prompt(chat_meta_info_t& meta_info, lm_uniform_input_t& input, int length_limit, std::ostream& os) {
    if (!this->insert(meta_info, input)) {
        return "";
    }
    return this->_shared_generate(meta_info, length_limit, os);
}
