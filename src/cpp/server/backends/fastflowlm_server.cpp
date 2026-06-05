#include "lemon/backends/fastflowlm_server.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/system_info.h"
#include "lemon/error_types.h"
#include "lemon/utils/process_manager.h"
#include "lemon/utils/http_client.h"
#include "lemon/utils/path_utils.h"
#include "lemon/utils/json_utils.h"
#include <iostream>
#include <filesystem>
#include <cstdlib>
#include <thread>
#include <chrono>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <lemon/utils/aixlog.hpp>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/wait.h>
#endif

namespace fs = std::filesystem;

namespace lemon {
namespace backends {

// URL to direct users to for driver updates
static const std::string DRIVER_INSTALL_URL = "https://lemonade-server.ai/driver_install.html";


InstallParams FastFlowLMServer::get_install_params(const std::string& backend, const std::string& version) {
    InstallParams params;

    if (backend == "system") {
        return params;
    }

    params.repo = "FastFlowLM/FastFlowLM";

    // Release asset filenames use bare version numbers (no 'v' prefix)
    std::string bare_version = version;
    if (!bare_version.empty() && bare_version[0] == 'v') {
        bare_version = bare_version.substr(1);
    }

#ifdef _WIN32
    params.filename = "fastflowlm_" + bare_version + "_windows_amd64.zip";
#else
    // On Linux, FLM must be installed as a system package by the user.
    throw std::runtime_error(
        "FLM auto-install is only supported on Windows. "
        "On Linux, install FLM manually: "
        "https://github.com/FastFlowLM/FastFlowLM/releases/tag/" + version);
#endif

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
    LOG(INFO, "FastFlowLM") << "Pulling model with FLM: " << checkpoint << std::endl;

    std::string flm_path = get_flm_path();
    if (flm_path.empty()) {
        throw std::runtime_error("FLM not found");
    }

    std::vector<std::string> args = {"pull", checkpoint};
    if (!do_not_upgrade) {
        args.push_back("--force");
    }

    LOG(INFO, "ProcessManager") << "Starting process: \"" << flm_path << "\"";
    for (const auto& arg : args) {
        LOG(INFO, "ProcessManager") << " \"" << arg << "\"";
    }
    LOG(INFO, "ProcessManager") << std::endl;

    auto handle = utils::ProcessManager::start_process(flm_path, args, "", is_debug());

    int timeout_seconds = 300;
    LOG(INFO, "FastFlowLM") << "Waiting for model download to complete..." << std::endl;
    bool completed = false;
    int exit_code = -1;

#ifdef _WIN32
    DWORD wait_result = WaitForSingleObject(handle.handle, timeout_seconds * 1000);
    if (wait_result == WAIT_OBJECT_0) {
        DWORD win_exit_code;
        GetExitCodeProcess(handle.handle, &win_exit_code);
        exit_code = static_cast<int>(win_exit_code);
        completed = true;
    }
#else
    for (int i = 0; i < timeout_seconds * 10; ++i) {
        int status;
        pid_t result = waitpid(handle.pid, &status, WNOHANG);
        if (result > 0) {
            exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            completed = true;
            break;
        } else if (result < 0) {
            completed = true;
            exit_code = -1;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        if (i % 50 == 0 && i > 0) {
            LOG(INFO, "FastFlowLM") << "Still downloading... (" << (i/10) << "s elapsed)" << std::endl;
        }
    }
#endif

    if (!completed) {
        utils::ProcessManager::stop_process(handle);
        throw std::runtime_error("FLM pull timed out after " + std::to_string(timeout_seconds) + " seconds");
    }

    if (exit_code != 0) {
        LOG(ERROR, "FastFlowLM") << "FLM pull failed with exit code: " << exit_code << std::endl;
        throw std::runtime_error("FLM pull failed with exit code: " + std::to_string(exit_code));
    }

    LOG(INFO, "FastFlowLM") << "Model pull completed successfully" << std::endl;
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

#ifdef _WIN32
    backend_manager_->install_backend(SPEC.recipe, "npu");
#endif

    // Validate NPU hardware/drivers
    std::string flm_path = get_flm_path();
    std::string validate_error;
    if (!utils::run_flm_validate(flm_path, validate_error)) {
        throw std::runtime_error("FLM NPU validation failed: " + validate_error +
            "\nVisit " + DRIVER_INSTALL_URL + " for driver installation instructions.");
    }

    // Download model if needed
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
    return ErrorResponse::from_exception(
        UnsupportedOperationException("Responses API", "flm")
    );
}

json FastFlowLMServer::embeddings(const json& request) {
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
        float ttft = 0.0f;

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

std::string FastFlowLMServer::get_flm_path() {
#ifdef _WIN32
    try {
        std::string path = BackendUtils::get_backend_binary_path(SPEC, "npu");
        LOG(INFO, "FastFlowLM") << "Found flm at: " << path << std::endl;
        return path;
    } catch (const std::exception& e) {
        LOG(ERROR, "FastFlowLM") << "flm not found in install dir: " << e.what() << std::endl;
        return "";
    }
#else
    std::string flm_path = utils::find_flm_executable();
    if (!flm_path.empty()) {
        LOG(INFO, "FastFlowLM") << "Found flm at: " << flm_path << std::endl;
    } else {
        LOG(ERROR, "FastFlowLM") << "flm not found in PATH" << std::endl;
    }
    return flm_path;
#endif
}

} // namespace backends
} // namespace lemon
