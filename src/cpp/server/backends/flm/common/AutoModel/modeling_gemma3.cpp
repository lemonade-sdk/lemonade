/// \file gemma3.cpp
/// \brief gemma3 class
/// \author FastFlowLM Team
/// \date 2025-09-03
/// \version 0.9.24
/// \note This is a source file for the gemma3 class

#include "AutoModel/modeling_gemma3.hpp"

/************              Gemma3 family            **************/
Gemma3::Gemma3(xrt::device* npu_device_inst) : AutoModel(npu_device_inst, "Gemma3") {}

void Gemma3::load_model(std::string model_path, json model_info, int default_context_length, bool enable_preemption) {
    this->_shared_load_model(model_path, model_info, default_context_length, enable_preemption);

    this->q4nx = std::make_unique<Q4NX>(this->model_path);
    // model_type == gemma
    this->lm_engine = std::make_unique<gemma_npu>(*this->lm_config, this->npu.get(), this->MAX_L);

    this->lm_engine->load_weights(*this->q4nx);

    //free the q4nx
    this->q4nx.reset();
    this->lm_engine->clear_context();
    this->setup_tokenizer(model_path);
    this->sampler.reset();

    sampler_config config;
    config.top_k = 40;
    config.top_p = 0.95;
    config.min_p = 0.1;
    config.temperature = 0.8;
    config.rep_penalty = 1.05;
    config.freq_penalty = 1.05;

    this->set_sampler(config);
    for (size_t i = 0; i < PROFILER_TYPE_NUM; i++) {
        this->profiler_list[i].reset();
    }
}

void Gemma3::setup_tokenizer(std::string model_path) {
    auto tokenizer_config = this->_shared_setup_tokenizer(model_path);
}

std::string Gemma3::apply_chat_template(nlohmann::ordered_json& messages, nlohmann::ordered_json tools) {
    minja::chat_template_inputs inputs;
    inputs.add_generation_prompt = true;
    inputs.messages = messages;
    inputs.extra_context = this->extra_context;
    return this->chat_tmpl->apply(inputs);
}

bool Gemma3::insert(chat_meta_info_t& meta_info, lm_uniform_input_t& input, std::function<bool()> is_cancelled) {
    static constexpr int IMAGE_TOKEN_ID = 262144; // replace with actual image token ID
    this->profiler_list[TKOEN_ENCODE_TIME].start();
    std::string templated_text;
    if (input.messages.empty() && input.prompt.empty()) {
        header_print("WARNING", "No messages or prompt provided");
        return false;
    }
    if (!input.messages.empty()) { // already a formated messages, usually from REST API
        templated_text = this->apply_chat_template(input.messages);
        // std::cout << "Templated text after applying chat template: \n" << templated_text << std::endl; // Debug print
    }
    else if (!input.prompt.empty()) { // a pure text, usually from the cli
        nlohmann::ordered_json messages;
        if (input.images.size() > 0) {
            nlohmann::ordered_json content;
            content["role"] = "user";
            content["content"] = input.prompt;
            content["images"] = nlohmann::ordered_json::array();
            for (int i = 0; i < input.images.size(); i++) {
                content["images"].push_back(input.images[i]);
            }
            messages.push_back(content);
        }
        else {
            messages.push_back({ {"role", "user"}, {"content", input.prompt} });
        }
        templated_text = this->apply_chat_template(messages);
    }
    // process all images

    bytes pixel_values;
    if (!input.messages.empty()) {
        int total_images = 0;
        for (auto& message : input.messages){
            nlohmann::ordered_json::array_t images = message.value("images", nlohmann::ordered_json::array());
            if (images.size() > 0){
                total_images += images.size();
            }
        }
        header_print("FLM", "Total images: " << total_images);
        // temporary solution
        if (total_images > 0){
            pixel_values.resize(3 * 896 * 896 * sizeof(bf16) * total_images);
            uint8_t* pixel_values_ptr = pixel_values.data();
            for (auto& message : input.messages){
                nlohmann::ordered_json::array_t images = message.value("images", nlohmann::ordered_json::array());
                for (auto& image : images){
                    std::string image_str = image.get<std::string>();
                    bytes image_rgb = load_image_base64(image_str);
                    buffer<bf16> pv = preprocess_image(image_rgb);
                    memcpy(pixel_values_ptr, pv.data(), pv.size() * sizeof(bf16));
                    pixel_values_ptr += pv.size() * sizeof(bf16);
                }
            }
        }
    }
    else { // from cli, typically only one image, typically a file path
        if (input.images.size() > 0){
            pixel_values.resize(3 * 896 * 896 * sizeof(bf16) * input.images.size());
            uint8_t* pixel_values_ptr = pixel_values.data();
            uint8_t* pixel_values_base = pixel_values.data();
            auto start_time = std::chrono::high_resolution_clock::now();
            for (auto& image : input.images){
                bytes image_rgb = load_image(image);
                if (image_rgb.size() == 0){
                    header_print("FLM", "Error: Could not load image: " << image);
                    header_print("FLM", "Please check if the file exists and is readable.");
                    continue;
                }

                buffer<bf16> pv = preprocess_image(image_rgb);
                if (pv.size() == 0){
                    header_print("FLM", "Error: Could not preprocess image: " << image);
                    header_print("FLM", "Please check if the image is valid.");
                    continue;
                }
                memcpy(pixel_values_ptr, pv.data(), pv.size() * sizeof(bf16));
                pixel_values_ptr += pv.size() * sizeof(bf16);
            }
            // Shrink pixel_values to what was actually written so that any
            // failed image loads do not leave uninitialized trailing data
            // (which would misalign pixels vs. image tokens during prefill).
            // `bytes::resize` reallocates without preserving content, so copy
            // into a fresh buffer instead.
            const size_t written = static_cast<size_t>(pixel_values_ptr - pixel_values_base);
            if (written < pixel_values.size()) {
                if (written == 0) {
                    pixel_values = bytes();
                } else {
                    bytes trimmed(written);
                    memcpy(trimmed.data(), pixel_values.data(), written);
                    pixel_values = std::move(trimmed);
                }
            }
            auto end_time = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
            header_print("FLM", "Image loaded in " << duration.count() << "ms");
        }
    }
    std::vector<int> tokens = this->tokenizer->encode(templated_text);

    // To avoid redownloading the model, a temporary fix, shall be removed in the next big update on gemma
    // if (tokens[tokens.size() - 1] != 107){
    //     tokens.push_back(107);
    // }

    this->profiler_list[TKOEN_ENCODE_TIME].stop(tokens.size());

    // ----------------------------------------------------------------------
    // Prompt-cache aware image alignment.
    //
    // AutoModel::_shared_insert prefix-matches `tokens` against `token_history`
    // over the FULL length of `token_history`. If every token matches, it
    // erases that prefix before prefilling; otherwise it calls clear_context()
    // and skips nothing. We must NOT erase `tokens` here -- _shared_insert
    // needs the untrimmed sequence to run that very check. What we DO need to
    // fix up locally is `pixel_values` (which carries pixels for the WHOLE
    // prompt, including images from earlier turns that are already cached):
    // drop the fully-cached leading images so the surviving payload aligns
    // with the surviving image tokens after _shared_insert performs its own
    // erase.
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
    }

    // Total images currently held in pixel_values (each image occupies a
    // fixed 3*896*896 bf16 elements block).
    const size_t per_img_bytes = static_cast<size_t>(3) * 896 * 896 * sizeof(bf16);
    const size_t total_images_in_payload =
        per_img_bytes > 0 ? pixel_values.size() / per_img_bytes : 0;

    if (prefix_skip_count > 0 && total_images_in_payload > 0) {
        int skipped_image_tokens = 0;
        for (size_t i = 0; i < prefix_skip_count; i++) {
            if (tokens[i] == IMAGE_TOKEN_ID) skipped_image_tokens++;
        }
        int total_image_tokens = 0;
        for (int t : tokens) {
            if (t == IMAGE_TOKEN_ID) total_image_tokens++;
        }
        if (skipped_image_tokens > 0 && total_image_tokens > 0) {
            const int per_img_tokens =
                total_image_tokens / static_cast<int>(total_images_in_payload);
            if (per_img_tokens > 0) {
                const size_t skipped_imgs =
                    static_cast<size_t>(skipped_image_tokens / per_img_tokens);
                const size_t drop_bytes =
                    std::min(skipped_imgs * per_img_bytes, pixel_values.size());
                if (drop_bytes > 0) {
                    const size_t remaining = pixel_values.size() - drop_bytes;
                    if (remaining == 0) {
                        pixel_values = bytes();
                    } else {
                        bytes trimmed(remaining);
                        memcpy(trimmed.data(),
                               pixel_values.data() + drop_bytes,
                               remaining);
                        pixel_values = std::move(trimmed);
                    }
                    header_print("FLM",
                        "Prompt-cache hit: dropped " << skipped_imgs
                        << " cached image(s) from payload");
                }
            }
        }
    }

    // hardware
    void* payload = pixel_values.size() > 0 ? static_cast<void*>(&pixel_values) : nullptr;

    // find the last image token index, expressed relative to the tokens that
    // will SURVIVE _shared_insert's prefix erase (i.e. shifted by -prefix_skip_count).
    int last_image_token_index = -1;
    for (int i = static_cast<int>(prefix_skip_count); i < (int)tokens.size(); i++) {
        if (tokens[i] == IMAGE_TOKEN_ID) {
            last_image_token_index = i - static_cast<int>(prefix_skip_count);
        }
    }
    last_image_token_index++; // plus the end of image tokens

    return this->_shared_insert(meta_info, tokens, is_cancelled, payload, last_image_token_index);
}

// 106 eos
// 107 \n
std::string Gemma3::generate(chat_meta_info_t& meta_info, int length_limit, std::ostream& os, std::function<bool()> is_cancelled) {
    return this->_shared_generate(meta_info, length_limit, os, is_cancelled);
}

std::string Gemma3::generate_with_prompt(chat_meta_info_t& meta_info, lm_uniform_input_t& input, int length_limit, std::ostream& os) {
    if (!this->insert(meta_info, input)) {
        return "";
    }
    return this->_shared_generate(meta_info, length_limit, os);
}
