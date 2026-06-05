#include "lemon/backends/fastflowlm_server.h"
#include "lemon/error_types.h"
#include <iostream>
#include <filesystem>
#include <sstream>
#include <lemon/utils/aixlog.hpp>

namespace lemon {
namespace backends {


InstallParams FastFlowLMServer::get_install_params(const std::string& backend, const std::string& version) {
    (void)backend;
    (void)version;
    InstallParams params;
    return params;
}

FastFlowLMServer::FastFlowLMServer(const std::string& log_level, ModelManager* model_manager,
                                   BackendManager* backend_manager)
    : WrappedServer("FastFlowLM", log_level, model_manager, backend_manager) {
}

FastFlowLMServer::~FastFlowLMServer() {
    unload();
}

std::string FastFlowLMServer::download_model(const std::string& checkpoint, bool do_not_upgrade) {
    (void)do_not_upgrade;
    // In-process FLM only: assume the model is already available locally.
    if (!std::filesystem::exists(checkpoint)) {
        throw std::runtime_error(
            "In-process FLM requires a local model path, but model was not found: " + checkpoint);
    }
    return checkpoint;
}

void FastFlowLMServer::load(const std::string& model_name,
                           const ModelInfo& model_info,
                           const RecipeOptions& options,
                           bool do_not_upgrade) {
    LOG(INFO, "FastFlowLM") << "Loading model: " << model_name << std::endl;

    int ctx_size = options.get_option("ctx_size");
    std::string flm_args = options.get_option("flm_args");

    std::cout << "[FastFlowLM] Options: ctx_size=" << ctx_size;
    if (!flm_args.empty()) {
        std::cout << ", flm_args=\"" << flm_args << "\"";
    }
    std::cout << std::endl;

    // Resolve model path for in-process FLM. No external FLM CLI is launched.
    download_model(model_info.checkpoint(), do_not_upgrade);

    // Initialize the in-process FLM engine
    LOG(INFO, "FastFlowLM") << "Initializing in-process FLM engine..." << std::endl;
    engine_ = std::make_unique<FlmEngine>();

    if (!engine_->init_device()) {
        throw std::runtime_error("Failed to initialize FLM NPU device");
    }

    // Build model configuration
    FlmModelConfig config;
    config.model_path = model_info.checkpoint();
    config.context_length = ctx_size > 0 ? ctx_size : 8192;
    config.temperature = 0.8f;
    config.top_k = 40;
    config.top_p = 0.9f;
    config.max_tokens = 1024;
    config.img_pre_resize = -1;

    // Parse flm_args for additional config
    if (!flm_args.empty()) {
        std::istringstream iss(flm_args);
        std::string token;
        while (iss >> token) {
            // Simple flag parsing
            if (token == "--preemption") {
                config.preemption = true;
            }
        }
    }

    try {
        engine_->load_model(config);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("FLM model load failed: ") + e.what());
    }

    current_model_tag_ = model_info.checkpoint();
    is_loaded_ = true;
    LOG(INFO, "FastFlowLM") << "Model loaded in-process: " << model_name << std::endl;
}

void FastFlowLMServer::unload() {
    LOG(INFO, "FastFlowLM") << "Unloading model..." << std::endl;
    if (is_loaded_ && engine_) {
        engine_->unload_model();
        engine_.reset();
        port_ = 0;
        is_loaded_ = false;
    }
}

json FastFlowLMServer::build_chat_response(const std::string& model,
                                           const FlmInferenceResult& result,
                                           int prompt_tokens,
                                           int completion_tokens) {
    nlohmann::ordered_json choices = nlohmann::ordered_json::array();
    nlohmann::ordered_json choice;
    choice["index"] = 0;
    choice["finish_reason"] = result.stop_reason;

    nlohmann::ordered_json message;
    message["role"] = "assistant";
    message["content"] = result.content;
    choice["message"] = message;
    choices.push_back(choice);

    nlohmann::ordered_json usage;
    usage["prompt_tokens"] = prompt_tokens;
    usage["completion_tokens"] = completion_tokens;
    usage["total_tokens"] = prompt_tokens + completion_tokens;
    usage["kv_token_occupancy_rate_percentage"] = 0.0;
    usage["load_duration"] = 0.0;
    usage["prefill_duration_ttft"] = result.prefill_duration_ms / 1000.0;
    usage["decoding_duration"] = result.decoding_duration_ms / 1000.0;
    usage["prefill_speed_tps"] = prompt_tokens > 0 ?
        (double)prompt_tokens / (result.prefill_duration_ms / 1000.0) : 0.0;
    usage["decoding_speed_tps"] = completion_tokens > 0 ?
        (double)completion_tokens / (result.decoding_duration_ms / 1000.0) : 0.0;

    return {
        {"id", "fastflowlm-chat-completion"},
        {"object", "chat.completion"},
        {"created", (long long)std::time(nullptr)},
        {"model", model},
        {"choices", choices},
        {"usage", usage},
        {"service_tier", "default"}
    };
}

json FastFlowLMServer::build_completion_response(const std::string& model,
                                                  const std::string& text,
                                                  int prompt_tokens,
                                                  int completion_tokens) {
    return {
        {"id", "fastflowlm-chat-completion"},
        {"object", "text_completion"},
        {"created", (int)std::time(nullptr)},
        {"model", model},
        {"choices", nlohmann::json::array({
            {
                {"text", text},
                {"index", 0},
                {"logprobs", nullptr},
                {"finish_reason", "stop"}
            }
        })},
        {"usage", {
            {"prompt_tokens", prompt_tokens},
            {"completion_tokens", completion_tokens},
            {"total_tokens", prompt_tokens + completion_tokens}
        }}
    };
}

json FastFlowLMServer::chat_completion(const json& request) {
    if (model_type_ == ModelType::TRANSCRIPTION || model_type_ == ModelType::EMBEDDING) {
        return ErrorResponse::from_exception(
            UnsupportedOperationException("Chat completion",
                "FLM " + model_type_to_string(model_type_) + " model")
        );
    }

    if (!engine_ || !engine_->is_model_loaded()) {
        return json{{"error", json{{"message", "Model not loaded"}, {"type", "server_error"}}}};
    }

    try {
        std::string model = request.value("model", current_model_tag_);
        auto& messages = request["messages"];
        auto tools = request.value("tools", json::array());
        auto extra = request.value("options", json::object());
        if (request.contains("max_tokens")) extra["max_tokens"] = request["max_tokens"];
        if (request.contains("max_completion_tokens")) extra["max_tokens"] = request["max_completion_tokens"];

        // Set temperature from request
        if (request.contains("temperature")) {
            extra["temperature"] = request["temperature"];
        }
        if (request.contains("top_p")) {
            extra["top_p"] = request["top_p"];
        }
        if (request.contains("top_k")) {
            extra["top_k"] = request["top_k"];
        }

        FlmInferenceResult result = engine_->chat_completion(messages, tools, extra);

        return build_chat_response(model, result,
            result.prompt_tokens, result.generated_tokens);

    } catch (const std::exception& e) {
        return json{{"error", json{
            {"message", std::string("Chat completion failed: ") + e.what()},
            {"type", "server_error"},
            {"code", 500}
        }}};
    }
}

json FastFlowLMServer::completion(const json& request) {
    if (model_type_ == ModelType::TRANSCRIPTION || model_type_ == ModelType::EMBEDDING) {
        return ErrorResponse::from_exception(
            UnsupportedOperationException("Text completion",
                "FLM " + model_type_to_string(model_type_) + " model")
        );
    }

    if (!engine_ || !engine_->is_model_loaded()) {
        return json{{"error", json{{"message", "Model not loaded"}, {"type", "server_error"}}}};
    }

    try {
        std::string model = request.value("model", current_model_tag_);
        std::string prompt = request["prompt"];
        auto extra = request.value("options", json::object());
        if (request.contains("max_tokens")) extra["max_tokens"] = request["max_tokens"];

        FlmInferenceResult result = engine_->text_completion(prompt, extra);

        return build_completion_response(model, result.content,
            result.prompt_tokens, result.generated_tokens);

    } catch (const std::exception& e) {
        return json{{"error", json{
            {"message", std::string("Completion failed: ") + e.what()},
            {"type", "server_error"},
            {"code", 500}
        }}};
    }
}

json FastFlowLMServer::responses(const json& request) {
    (void)request;
    return ErrorResponse::from_exception(
        UnsupportedOperationException("Responses API", "flm")
    );
}

json FastFlowLMServer::embeddings(const json& request) {
    (void)request;
    if (model_type_ == ModelType::TRANSCRIPTION) {
        return ErrorResponse::from_exception(
            UnsupportedOperationException("Embeddings",
                "FLM " + model_type_to_string(model_type_) + " model")
        );
    }
    // Embeddings are not directly supported in the in-process engine yet.
    // Return a placeholder error.
    return json{{"error", json{
        {"message", "Embeddings not yet implemented for in-process FLM backend"},
        {"type", "server_error"},
        {"code", 501}
    }}};
}

json FastFlowLMServer::reranking(const json& request) {
    (void)request;
    if (model_type_ != ModelType::LLM) {
        return ErrorResponse::from_exception(
            UnsupportedOperationException("Reranking",
                "FLM " + model_type_to_string(model_type_) + " model")
        );
    }
    return json{{"error", json{
        {"message", "Reranking not yet implemented for in-process FLM backend"},
        {"type", "server_error"},
        {"code", 501}
    }}};
}

json FastFlowLMServer::audio_transcriptions(const json& request) {
    if (model_type_ != ModelType::TRANSCRIPTION) {
        return ErrorResponse::from_exception(
            UnsupportedOperationException("Audio transcription",
                "FLM " + model_type_to_string(model_type_) + " model")
        );
    }

    try {
        if (!request.contains("file_data")) {
            throw std::runtime_error("Missing 'file_data' in request");
        }

        std::string audio_data = request["file_data"].get<std::string>();
        std::string filename = request.value("filename", "audio.wav");

        // Audio transcription requires the ASR model path to be set.
        // For now, return a placeholder.
        return json{{"error", json{
            {"message", "Audio transcription requires ASR model — not yet implemented for in-process FLM"},
            {"type", "server_error"},
            {"code", 501}
        }}};

    } catch (const std::exception& e) {
        return json{{"error", json{
            {"message", std::string("Transcription failed: ") + e.what()},
            {"type", "audio_processing_error"}
        }}};
    }
}

void FastFlowLMServer::forward_streaming_request(const std::string& endpoint,
                                                  const std::string& request_body,
                                                  httplib::DataSink& sink,
                                                  bool sse,
                                                  long timeout_seconds) {
    // Determine which endpoint is being called
    if (endpoint == "/v1/chat/completions") {
        stream_chat_completion(json::parse(request_body), sink);
    } else if (endpoint == "/v1/completions") {
        stream_completion(json::parse(request_body), sink);
    } else {
        // Fall back to base class forwarding (for unsupported endpoints)
        WrappedServer::forward_streaming_request(endpoint, request_body, sink, sse, timeout_seconds);
    }
}

void FastFlowLMServer::stream_chat_completion(const json& request, httplib::DataSink& sink) {
    if (!engine_ || !engine_->is_model_loaded()) {
        std::string error = "data: {\"error\":{\"message\":\"Model not loaded\",\"type\":\"server_error\"}}\n\n";
        sink.write(error.c_str(), error.size());
        return;
    }

    try {
        std::string model = request.value("model", current_model_tag_);
        auto& messages = request["messages"];
        auto tools = request.value("tools", json::array());
        auto extra = request.value("options", json::object());
        if (request.contains("max_tokens")) extra["max_tokens"] = request["max_tokens"];
        if (request.contains("max_completion_tokens")) extra["max_tokens"] = request["max_completion_tokens"];

        // Collect streaming output
        std::string full_content;
        int prompt_tokens = 0, completion_tokens = 0;
        auto callback = [&](const std::string& chunk, bool is_final) {
            if (!chunk.empty()) {
                full_content += chunk;
                completion_tokens++;

                json delta;
                delta["role"] = "assistant";
                delta["content"] = chunk;

                json sse_data = {
                    {"id", "fastflowlm-chat-completion"},
                    {"object", "chat.completion.chunk"},
                    {"created", (long long)std::time(nullptr)},
                    {"model", model},
                    {"choices", nlohmann::json::array({
                        {"index", 0, "delta", delta, "finish_reason", nullptr}
                    })}
                };

                std::string sse_line = "data: " + sse_data.dump() + "\n\n";
                sink.write(sse_line.c_str(), sse_line.size());
            }

            if (is_final) {
                // Send final SSE event with usage
                json final_chunk;
                final_chunk["index"] = 0;
                final_chunk["delta"] = json::object();
                final_chunk["finish_reason"] = "stop";

                json sse_final = {
                    {"id", "fastflowlm-chat-completion"},
                    {"object", "chat.completion.chunk"},
                    {"created", (long long)std::time(nullptr)},
                    {"model", model},
                    {"choices", nlohmann::json::array({
                        {"index", 0, "delta", final_chunk, "finish_reason", "stop"}
                    })},
                    {"usage", {
                        {"prompt_tokens", prompt_tokens},
                        {"completion_tokens", completion_tokens},
                        {"total_tokens", prompt_tokens + completion_tokens}
                    }}
                };

                std::string sse_final_line = "data: " + sse_final.dump() + "\n\n";
                sink.write(sse_final_line.c_str(), sse_final_line.size());
                sink.done();
            }
        };

        engine_->chat_completion_streaming(messages, callback, tools, extra);
        // Note: full_content is collected via the callback above

    } catch (const std::exception& e) {
        json error = {
            {"error", json{
                {"message", std::string("Streaming chat failed: ") + e.what()},
                {"type", "server_error"},
                {"code", 500}
            }}
        };
        std::string error_line = "data: " + error.dump() + "\n\n";
        sink.write(error_line.c_str(), error_line.size());
        sink.done();
    }
}

void FastFlowLMServer::stream_completion(const json& request, httplib::DataSink& sink) {
    if (!engine_ || !engine_->is_model_loaded()) {
        std::string error = "data: {\"error\":{\"message\":\"Model not loaded\",\"type\":\"server_error\"}}\n\n";
        sink.write(error.c_str(), error.size());
        return;
    }

    try {
        std::string model = request.value("model", current_model_tag_);
        std::string prompt = request["prompt"];
        auto extra = request.value("options", json::object());
        if (request.contains("max_tokens")) extra["max_tokens"] = request["max_tokens"];

        int completion_tokens = 0;

        auto callback = [&](const std::string& chunk, bool is_final) {
            if (!chunk.empty()) {
                completion_tokens++;
                json sse_data = {
                    {"id", "fastflowlm-chat-completion"},
                    {"object", "text_completion.chunk"},
                    {"created", (int)std::time(nullptr)},
                    {"model", model},
                    {"choices", nlohmann::json::array({
                        {"index", 0, "text", chunk, "finish_reason", nullptr}
                    })}
                };
                std::string sse_line = "data: " + sse_data.dump() + "\n\n";
                sink.write(sse_line.c_str(), sse_line.size());
            }

            if (is_final) {
                json sse_final = {
                    {"id", "fastflowlm-chat-completion"},
                    {"object", "text_completion.chunk"},
                    {"created", (int)std::time(nullptr)},
                    {"model", model},
                    {"choices", nlohmann::json::array({
                        {"index", 0, "text", "", "finish_reason", "stop"}
                    })}
                };
                std::string sse_final_line = "data: " + sse_final.dump() + "\n\n";
                sink.write(sse_final_line.c_str(), sse_final_line.size());
                sink.done();
            }
        };

        engine_->text_completion_streaming(prompt, callback, extra);

    } catch (const std::exception& e) {
        json error = {
            {"error", json{
                {"message", std::string("Streaming completion failed: ") + e.what()},
                {"type", "server_error"},
                {"code", 500}
            }}
        };
        std::string error_line = "data: " + error.dump() + "\n\n";
        sink.write(error_line.c_str(), error_line.size());
        sink.done();
    }
}

} // namespace backends
} // namespace lemon
