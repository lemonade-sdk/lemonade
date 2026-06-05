/// \file flm_engine.cpp
/// \brief Implementation of the FlmEngine high-level FLM inference interface.
/// Bridges the FLM internal API (AutoModel, NPU device, tokenizer) with a
/// clean, OpenAI-compatible interface that lemonade's backend uses.
#include "flm_engine.h"
#include "npu_utils/npu_utils.hpp"
#include "AutoModel/automodel.hpp"
#include "AutoModel/all_models.hpp"
#include "model_list.hpp"
#include "tokenizer/tokenizer.hpp"
#include "modules/sampler.hpp"
#include "utils/utils.hpp"
#include "utils/profiler.hpp"
#include "prompt_cache.hpp"
#include "minja/chat-template.hpp"
#include <nlohmann/json.hpp>
#include <sstream>
#include <iostream>
#include <thread>
#include <chrono>
#include <algorithm>
#include <cstring>

namespace lemonade::backends {

// ============================================================
// Helper: normalize messages (from rest_handler.cpp)
// ============================================================

static nlohmann::ordered_json normalize_messages(nlohmann::ordered_json messages) {
    if (messages.empty()) return messages;

    nlohmann::ordered_json normalized = nlohmann::ordered_json::array();

    for (size_t i = 0; i < messages.size(); i++) {
        auto current_msg = messages[i];
        std::string role = current_msg.value("role", "");

        if (role == "user") {
            nlohmann::ordered_json merged_content_array = nlohmann::ordered_json::array();

            while (i < messages.size() && messages[i].value("role", "") == "user") {
                if (messages[i].contains("content")) {
                    if (messages[i]["content"].is_array()) {
                        for (auto& item : messages[i]["content"]) {
                            merged_content_array.push_back(item);
                        }
                    } else if (messages[i]["content"].is_string()) {
                        std::string text = messages[i]["content"].get<std::string>();
                        if (!text.empty()) {
                            nlohmann::ordered_json text_item;
                            text_item["type"] = "text";
                            text_item["text"] = text;
                            merged_content_array.push_back(text_item);
                        }
                    }
                }
                if (i + 1 < messages.size() && messages[i + 1].value("role", "") == "user") i++;
                else break;
            }

            current_msg["content"] = merged_content_array;
        } else if (role == "system") {
            nlohmann::ordered_json merged_content_array = nlohmann::ordered_json::array();

            while (i < messages.size() && messages[i].value("role", "") == "system") {
                if (messages[i].contains("content")) {
                    if (messages[i]["content"].is_array()) {
                        for (auto& item : messages[i]["content"]) {
                            merged_content_array.push_back(item);
                        }
                    } else if (messages[i]["content"].is_string()) {
                        std::string text = messages[i]["content"].get<std::string>();
                        if (!text.empty()) {
                            nlohmann::ordered_json text_item;
                            text_item["type"] = "text";
                            text_item["text"] = text;
                            merged_content_array.push_back(text_item);
                        }
                    }
                }
                if (i + 1 < messages.size() && messages[i + 1].value("role", "") == "system") i++;
                else break;
            }

            current_msg["content"] = merged_content_array;
        } else if (role == "assistant") {
            if (current_msg.contains("thinking")) current_msg.erase("thinking");
            if (current_msg.contains("reasoning")) current_msg.erase("reasoning");
            if (current_msg.contains("reasoning_content")) current_msg.erase("reasoning_content");
        }

        normalized.push_back(current_msg);
    }

    return normalized;
}

static nlohmann::ordered_json normalize_template(nlohmann::ordered_json messages) {
    nlohmann::ordered_json template_message = nlohmann::ordered_json::array();

    for (auto& message : messages) {
        nlohmann::ordered_json new_message = message;
        std::string merged_text;
        nlohmann::ordered_json::array_t merged_images;
        nlohmann::ordered_json::array_t merged_audio;

        if (message["content"].is_string()) {
            merged_text = message["content"].get<std::string>();
        } else if (message["content"].is_array()) {
            for (auto& contentItem : message["content"]) {
                if (contentItem.contains("type") && contentItem["type"] == "text") {
                    merged_text += contentItem["text"].get<std::string>();
                } else if (contentItem.contains("type") && contentItem["type"] == "image_url") {
                    std::string image_url = contentItem["image_url"]["url"].get<std::string>();
                    const std::vector<std::string> prefixes = {
                        "data:image/png;base64,",
                        "data:image/jpeg;base64,",
                        "data:image/jpg;base64,"
                    };
                    for (const auto& prefix : prefixes) {
                        if (image_url.substr(0, prefix.length()) == prefix) {
                            image_url = image_url.substr(prefix.length());
                            break;
                        }
                    }
                    if (image_url.empty()) continue;
                    merged_images.push_back(image_url);
                } else if (contentItem.contains("type") && contentItem["type"] == "input_audio") {
                    std::string audio_base64 = contentItem["input_audio"]["data"].get<std::string>();
                    if (!audio_base64.empty()) {
                        merged_audio.push_back(audio_base64);
                    }
                }
            }
        }

        new_message["content"] = merged_text;
        if (!merged_images.empty()) new_message["images"] = merged_images;
        if (!merged_audio.empty()) new_message["audios"] = merged_audio;

        template_message.push_back(new_message);
    }

    return template_message;
}

// ============================================================
// Helper: build OpenAI-style completion response
// ============================================================

static nlohmann::ordered_json build_openai_chat_response(
    const std::string& model,
    const std::string& content,
    const chat_meta_info_t& meta_info,
    const std::string& stop_reason_str)
{
    nlohmann::ordered_json choices = nlohmann::ordered_json::array();
    nlohmann::ordered_json choice;
    choice["index"] = 0;
    choice["finish_reason"] = stop_reason_str;

    nlohmann::ordered_json message;
    message["role"] = "assistant";
    message["content"] = content;
    choice["message"] = message;
    choices.push_back(choice);

    nlohmann::ordered_json usage;
    usage["prompt_tokens"] = meta_info.prompt_tokens;
    usage["completion_tokens"] = meta_info.generated_tokens;
    usage["total_tokens"] = meta_info.prompt_tokens + meta_info.generated_tokens;
    usage["kv_token_occupancy_rate_percentage"] =
        (float)0.0 / 8192.0f * 100.0f; // placeholder, actual value set below

    return {
        {"id", "fastflowlm-chat-completion"},
        {"object", "chat.completion"},
        {"created", (long long)std::time(nullptr)},
        {"model", model},
        {"choices", choices},
        {"usage", usage}
    };
}

// ============================================================
// NpuDevice implementation
// ============================================================

class NpuDevice::Impl {
public:
    xrt::device device;
    Impl() : device(0) {}
};

std::unique_ptr<NpuDevice> NpuDevice::create() {
    try {
        auto dev = std::unique_ptr<NpuDevice>(new NpuDevice());
        dev->impl_ = std::make_unique<Impl>();
        return dev;
    } catch (const std::exception& e) {
        std::cerr << "FLM: Failed to initialize NPU device: " << e.what() << std::endl;
        return nullptr;
    }
}

NpuDevice::NpuDevice() : impl_(nullptr) {}

void* NpuDevice::raw_device() const {
    return impl_ ? &impl_->device : nullptr;
}

// ============================================================
// FlmEngine implementation
// ============================================================

class FlmEngine::Impl {
public:
    std::unique_ptr<NpuDevice> npu_device;
    std::unique_ptr<AutoModel> auto_model;
    model_list model_registry;
    std::string current_model_tag;
    int ctx_length = -1;
    int prefill_chunk_len = 512;
    int img_pre_resize = -1;
    bool preemption = false;
    bool asr = false;
    bool embed = false;
    std::function<bool()> cancel_callback = [] { return false; };
    PromptCache prompt_cache;
    std::string model_used_for_last_message;

    // Sampler config
    float temperature = 0.8f;
    int top_k = 40;
    float top_p = 0.9f;
    float repetition_penalty = 1.0f;
    float frequency_penalty = 0.0f;
    float presence_penalty = 0.0f;
    int max_tokens = 1024;
    int min_tokens = 0;

    // System prompt
    std::string system_prompt;

    Impl() : npu_device(NpuDevice::create()) {}
};

FlmEngine::FlmEngine() : impl_(std::make_unique<Impl>()) {}

FlmEngine::~FlmEngine() = default;

bool FlmEngine::init_device() {
    if (!impl_->npu_device) {
        return false;
    }
    return true;
}

bool FlmEngine::load_model(const FlmModelConfig& config) {
    if (!impl_->npu_device) {
        throw std::runtime_error("NPU device not initialized. Call init_device() first.");
    }

    // Check if model is supported
    if (!impl_->model_registry.is_model_supported(config.model_path)) {
        // Try using the model_path as a tag
        if (!impl_->model_registry.is_model_supported(config.model_path)) {
            // Try to find it by scanning the model_list.json
            // For now, try loading with the path as-is
            std::cerr << "FLM: Model tag not in model list, trying direct path: " << config.model_path << std::endl;
        }
    }

    // Unload current model if switching
    if (impl_->auto_model) {
        impl_->auto_model.reset();
    }

    // Get model info
    auto [resolved_tag, model_info] = impl_->model_registry.get_model_info(config.model_path);
    impl_->current_model_tag = resolved_tag;
    impl_->model_used_for_last_message = resolved_tag;

    // Get model path from model list
    std::string model_path = impl_->model_registry.get_model_path(resolved_tag);
    if (model_path.empty()) {
        model_path = config.model_path;
    }

    // Create the AutoModel instance
    void* raw_dev = impl_->npu_device->raw_device();
    xrt::device* dev_ptr = static_cast<xrt::device*>(raw_dev);

    try {
        auto auto_model = get_auto_model(resolved_tag, impl_->model_registry, dev_ptr);
        impl_->auto_model = std::move(auto_model.second);
        impl_->current_model_tag = auto_model.first;
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Failed to create AutoModel: ") + e.what());
    }

    // Configure model
    try {
        impl_->auto_model->configure_parameter("img_pre_resize", config.img_pre_resize);
        impl_->auto_model->load_model(model_path, model_info,
                                       config.context_length > 0 ? config.context_length : impl_->ctx_length,
                                       config.preemption);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Failed to load model: ") + e.what());
    }

    // Configure sampler
    sampler_config sampler_cfg;
    sampler_cfg.temperature = config.temperature;
    sampler_cfg.top_k = config.top_k;
    sampler_cfg.top_p = config.top_p;
    sampler_cfg.rep_penalty = config.repetition_penalty;
    impl_->auto_model->set_sampler(sampler_cfg);
    impl_->auto_model->set_max_length(config.max_tokens);
    impl_->auto_model->set_topk(config.top_k);
    impl_->auto_model->set_topp(config.top_p);
    impl_->auto_model->set_temperature(config.temperature);
    impl_->auto_model->set_presence_penalty(config.presence_penalty);
    impl_->auto_model->set_repetition_penalty(config.repetition_penalty);
    impl_->auto_model->set_frequency_penalty(config.frequency_penalty);

    // Set system prompt if provided
    if (!config.system_prompt.empty()) {
        impl_->auto_model->configure_parameter("system_prompt", config.system_prompt);
    }

    return true;
}

void FlmEngine::unload_model() {
    if (impl_->auto_model) {
        impl_->auto_model->clear_context();
        impl_->auto_model.reset();
    }
    impl_->current_model_tag.clear();
    impl_->prompt_cache.reset();
}

FlmInferenceResult FlmEngine::chat_completion(
    const nlohmann::json& messages,
    const nlohmann::json& tools,
    const nlohmann::json& extra)
{
    if (!impl_->auto_model) {
        throw std::runtime_error("No model loaded. Call load_model() first.");
    }

    // Normalize messages
    auto norm_messages = normalize_messages(messages);
    norm_messages = normalize_template(norm_messages);

    chat_meta_info_t meta_info;
    meta_info.max_prefill_len = impl_->prefill_chunk_len;

    lm_uniform_input_t uniformed_input;
    uniformed_input.messages = norm_messages;
    uniformed_input.tools = tools;

    // Prefill
    impl_->auto_model->reset_parser();
    try {
        bool success = impl_->auto_model->insert(meta_info, uniformed_input, impl_->cancel_callback);
        if (!success) {
            if (meta_info.stop_reason == CANCEL_DETECTED) {
                throw std::runtime_error("Generation cancelled");
            }
            throw std::runtime_error("Max length reached during prefill");
        }
    } catch (...) {
        impl_->auto_model->clear_context();
        throw;
    }

    // Generate
    std::stringstream ss;

    int length_limit = extra.value("max_tokens", impl_->max_tokens);
    try {
        impl_->auto_model->generate(meta_info, length_limit, ss, impl_->cancel_callback);
    } catch (...) {
        impl_->auto_model->clear_context();
        throw;
    }

    std::string response_text = ss.str();

    // Parse response
    NonStreamResult parsed = impl_->auto_model->parse_nstream_content(response_text);

    // Build result
    FlmInferenceResult result;
    result.content = parsed.content;
    result.reasoning_content = parsed.reasoning_content;
    result.stop_reason = stop_reason_to_string(meta_info.stop_reason);
    result.prompt_tokens = meta_info.prompt_tokens;
    result.generated_tokens = meta_info.generated_tokens;
    result.ttft_seconds = impl_->auto_model->get_ttft();
    result.total_duration_ms = meta_info.total_duration / 1'000'000;
    result.prefill_duration_ms = meta_info.prefill_duration / 1'000'000;
    result.decoding_duration_ms = meta_info.decoding_duration / 1'000'000;

    // Clear context for next request
    impl_->auto_model->clear_context();

    return result;
}

FlmInferenceResult FlmEngine::chat_completion_streaming(
    const nlohmann::json& messages,
    FlmStreamCallback callback,
    const nlohmann::json& tools,
    const nlohmann::json& extra)
{
    if (!impl_->auto_model) {
        throw std::runtime_error("No model loaded. Call load_model() first.");
    }

    // Normalize messages
    auto norm_messages = normalize_messages(messages);
    norm_messages = normalize_template(norm_messages);

    chat_meta_info_t meta_info;
    meta_info.max_prefill_len = impl_->prefill_chunk_len;

    lm_uniform_input_t uniformed_input;
    uniformed_input.messages = norm_messages;
    uniformed_input.tools = tools;

    // Prefill
    impl_->auto_model->reset_parser();
    try {
        bool success = impl_->auto_model->insert(meta_info, uniformed_input, impl_->cancel_callback);
        if (!success) {
            if (meta_info.stop_reason == CANCEL_DETECTED) {
                throw std::runtime_error("Generation cancelled");
            }
            throw std::runtime_error("Max length reached during prefill");
        }
    } catch (...) {
        impl_->auto_model->clear_context();
        throw;
    }

    // chat_completion_streaming
    // Generate with streaming (API uses std::ostream&, accumulate then callback)
    int length_limit = extra.value("max_tokens", impl_->max_tokens);
    std::stringstream ss;
    try {
        impl_->auto_model->generate(meta_info, length_limit, ss, impl_->cancel_callback);
    } catch (...) {
        impl_->auto_model->clear_context();
        throw;
    }

    std::string output = ss.str();
    // Invoke callback with accumulated output
    callback(output, true);

    // Build final result from meta_info
    FlmInferenceResult result;
    result.content = ""; // Streaming doesn't accumulate in this simple impl
    result.stop_reason = stop_reason_to_string(meta_info.stop_reason);
    result.prompt_tokens = meta_info.prompt_tokens;
    result.generated_tokens = meta_info.generated_tokens;
    result.ttft_seconds = impl_->auto_model->get_ttft();
    result.total_duration_ms = meta_info.total_duration / 1'000'000;
    result.prefill_duration_ms = meta_info.prefill_duration / 1'000'000;
    result.decoding_duration_ms = meta_info.decoding_duration / 1'000'000;

    impl_->auto_model->clear_context();

    return result;
}

FlmInferenceResult FlmEngine::text_completion(
    const std::string& prompt,
    const nlohmann::json& extra)
{
    if (!impl_->auto_model) {
        throw std::runtime_error("No model loaded. Call load_model() first.");
    }

    chat_meta_info_t meta_info;
    meta_info.max_prefill_len = impl_->prefill_chunk_len;

    lm_uniform_input_t uniformed_input;
    uniformed_input.prompt = prompt;

    // Prefill
    try {
        bool success = impl_->auto_model->insert(meta_info, uniformed_input, impl_->cancel_callback);
        if (!success) {
            if (meta_info.stop_reason == CANCEL_DETECTED) {
                throw std::runtime_error("Generation cancelled");
            }
            throw std::runtime_error("Max length reached during prefill");
        }
    } catch (...) {
        impl_->auto_model->clear_context();
        throw;
    }

    // Generate
    std::stringstream ss;

    int length_limit = extra.value("max_tokens", impl_->max_tokens);
    try {
        impl_->auto_model->generate(meta_info, length_limit, ss, impl_->cancel_callback);
    } catch (...) {
        impl_->auto_model->clear_context();
        throw;
    }

    std::string response_text = ss.str();

    FlmInferenceResult result;
    result.content = response_text;
    result.stop_reason = stop_reason_to_string(meta_info.stop_reason);
    result.prompt_tokens = meta_info.prompt_tokens;
    result.generated_tokens = meta_info.generated_tokens;
    result.ttft_seconds = impl_->auto_model->get_ttft();
    result.total_duration_ms = meta_info.total_duration / 1'000'000;
    result.prefill_duration_ms = meta_info.prefill_duration / 1'000'000;
    result.decoding_duration_ms = meta_info.decoding_duration / 1'000'000;

    impl_->auto_model->clear_context();

    return result;
}

FlmInferenceResult FlmEngine::text_completion_streaming(
    const std::string& prompt,
    FlmStreamCallback callback,
    const nlohmann::json& extra)
{
    if (!impl_->auto_model) {
        throw std::runtime_error("No model loaded. Call load_model() first.");
    }

    chat_meta_info_t meta_info;
    meta_info.max_prefill_len = impl_->prefill_chunk_len;

    lm_uniform_input_t uniformed_input;
    uniformed_input.prompt = prompt;

    // Prefill
    try {
        bool success = impl_->auto_model->insert(meta_info, uniformed_input, impl_->cancel_callback);
        if (!success) {
            if (meta_info.stop_reason == CANCEL_DETECTED) {
                throw std::runtime_error("Generation cancelled");
            }
            throw std::runtime_error("Max length reached during prefill");
        }
    } catch (...) {
        impl_->auto_model->clear_context();
        throw;
    }

   // text_completion_streaming
    // Generate with streaming (API uses std::ostream&, accumulate then callback)
    int length_limit = extra.value("max_tokens", impl_->max_tokens);
    std::stringstream ss;
    try {
        impl_->auto_model->generate(meta_info, length_limit, ss, impl_->cancel_callback);
    } catch (...) {
        impl_->auto_model->clear_context();
        throw;
    }

    std::string output = ss.str();
    // Invoke callback with accumulated output
    callback(output, true);

    FlmInferenceResult result;
    result.content = "";
    result.stop_reason = stop_reason_to_string(meta_info.stop_reason);
    result.prompt_tokens = meta_info.prompt_tokens;
    result.generated_tokens = meta_info.generated_tokens;

    impl_->auto_model->clear_context();

    return result;
}

std::string FlmEngine::current_model() const {
    return impl_->current_model_tag;
}

nlohmann::json FlmEngine::model_info() const {
    if (!impl_->auto_model) return nlohmann::json::object();
    return impl_->auto_model->show_model_info();
}

nlohmann::json FlmEngine::profile_data() const {
    if (!impl_->auto_model) return nlohmann::json::object();
    return impl_->auto_model->show_profile();
}

bool FlmEngine::is_model_loaded() const {
    return impl_->auto_model != nullptr;
}

std::vector<std::string> FlmEngine::supported_models() const {
    std::vector<std::string> tags;
    for (const auto& tag : impl_->model_registry.all_tags) {
        tags.push_back(tag);
    }
    return tags;
}

void FlmEngine::set_cancel_callback(std::function<bool()> cancel_fn) {
    impl_->cancel_callback = std::move(cancel_fn);
}

} // namespace lemonade::backends
