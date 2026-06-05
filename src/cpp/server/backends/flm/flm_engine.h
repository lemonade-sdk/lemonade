/// \file flm_engine.h
/// \brief High-level interface to the FastFlowLM inference engine.
/// Wraps NPU device management, model loading, and inference into a
/// clean C++ API that lemonade can call directly — no HTTP server needed.
#pragma once

#include <memory>
#include <string>
#include <functional>
#include <vector>
#include <optional>
#include <nlohmann/json.hpp>

namespace lemonade::backends {

/// Opaque handle to an NPU device context.
class NpuDevice {
public:
    /// Initialize the NPU device. Returns nullptr on failure.
    static std::unique_ptr<NpuDevice> create();

    /// Get the raw xrt::device pointer (for internal FLM use).
    void* raw_device() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
    NpuDevice();
    friend class FlmEngine;
};

/// Configuration for model loading.
struct FlmModelConfig {
    /// Path to the model directory (containing config.json, tokenizer, etc.)
    std::string model_path;

    /// Default context length (-1 = use model default)
    int context_length = -1;

    /// Enable preemption
    bool preemption = false;

    /// Sampler parameters
    float temperature = 0.8f;
    int top_k = 40;
    float top_p = 0.9f;
    float repetition_penalty = 1.0f;
    float frequency_penalty = 0.0f;
    float presence_penalty = 0.0f;
    int max_tokens = 1024;
    int min_tokens = 0;

    /// System prompt
    std::string system_prompt;

    /// Image preprocessing resize (-1 = auto)
    int img_pre_resize = -1;
};

/// Result of a single non-streaming inference call.
struct FlmInferenceResult {
    std::string content;
    std::string reasoning_content;
    std::string stop_reason;   // "stop", "length", "error", "tool_calls"
    int prompt_tokens = 0;
    int generated_tokens = 0;
    float ttft_seconds = 0.0f;
    float total_duration_ms = 0.0f;
    float prefill_duration_ms = 0.0f;
    float decoding_duration_ms = 0.0f;
};

/// Callback for streaming inference output.
/// \param chunk The text chunk
/// \param is_final Whether this is the final chunk
using FlmStreamCallback = std::function<void(const std::string& chunk, bool is_final)>;

/// High-level inference engine. Thread-safe for read-only operations;
/// model loading and inference must be called sequentially.
class FlmEngine {
public:
    FlmEngine();
    ~FlmEngine();

    /// Non-copyable, non-movable (owns NPU device).
    FlmEngine(const FlmEngine&) = delete;
    FlmEngine& operator=(const FlmEngine&) = delete;

    /// Initialize the NPU device. Must be called before load_model.
    /// Returns true on success.
    bool init_device();

    /// Load a model. Blocks until the model is ready for inference.
    /// \param config Model configuration
    /// \return true on success
    bool load_model(const FlmModelConfig& config);

    /// Unload the current model.
    void unload_model();

    /// Run a chat completion (non-streaming).
    /// \param messages Array of message objects with "role" and "content" fields
    /// \param tools Optional tool definitions (OpenAI format)
    /// \return Inference result
    FlmInferenceResult chat_completion(
        const nlohmann::json& messages,
        const nlohmann::json& tools = nlohmann::json::object(),
        const nlohmann::json& extra = nlohmann::json::object());

    /// Run a chat completion (streaming).
    /// \param messages Array of message objects
    /// \param callback Called for each token chunk
    /// \param tools Optional tool definitions
    /// \return Inference result (populated after streaming completes)
    FlmInferenceResult chat_completion_streaming(
        const nlohmann::json& messages,
        FlmStreamCallback callback,
        const nlohmann::json& tools = nlohmann::json::object(),
        const nlohmann::json& extra = nlohmann::json::object());

    /// Run a text completion (non-streaming).
    /// \param prompt The prompt text
    /// \return Inference result
    FlmInferenceResult text_completion(
        const std::string& prompt,
        const nlohmann::json& extra = nlohmann::json::object());

    /// Run a text completion (streaming).
    FlmInferenceResult text_completion_streaming(
        const std::string& prompt,
        FlmStreamCallback callback,
        const nlohmann::json& extra = nlohmann::json::object());

    /// Get the currently loaded model tag.
    std::string current_model() const;

    /// Get model info as JSON.
    nlohmann::json model_info() const;

    /// Get profiling data as JSON.
    nlohmann::json profile_data() const;

    /// Check if a model is loaded.
    bool is_model_loaded() const;

    /// Get list of supported model tags.
    std::vector<std::string> supported_models() const;

    /// Set a cancellation callback. Return true to cancel inference.
    void set_cancel_callback(std::function<bool()> cancel_fn);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lemonade::backends
