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
    // The FLM .deb bundles non-portable libraries (libxrt, ffmpeg) that
    // require system-level installation. Auto-install is Windows-only.
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

    // Use flm pull command to download the model
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

    // Run flm pull command (with debug output if enabled)
    auto handle = utils::ProcessManager::start_process(flm_path, args, "", is_debug());

    // Wait for process to complete (handles both fast exits and long downloads)
    // NOTE: On Linux, is_running() reaps the process via waitpid(), making the
    // exit code unavailable to get_exit_code(). Use WaitForSingleObject/waitpid
    // directly instead of the is_running/get_exit_code combo.
    int timeout_seconds = 300; // 5 minutes
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
            // Process doesn't exist or error
            completed = true;
            exit_code = -1;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Print progress every 5 seconds
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

    // Get FLM-specific options from RecipeOptions
    int ctx_size = options.get_option("ctx_size");
    std::string flm_args = options.get_option("flm_args");

    std::cout << "[FastFlowLM] Options: ctx_size=" << ctx_size;
    if (!flm_args.empty()) {
        std::cout << ", flm_args=\"" << flm_args << "\"";
    }
    std::cout << std::endl;
    // Note: checkpoint_ is set by Router via set_model_metadata() before load() is called
    // We use checkpoint_ (base class field) for FLM API calls

#ifdef _WIN32
    // On Windows, auto-install FLM binary if needed (downloads zip and extracts)
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

    // Choose a port
    port_ = choose_port();

    // Construct flm serve command based on model type
    // Bind to localhost only for security
    std::vector<std::string> args;
    if (model_type_ == ModelType::AUDIO) {
        // ASR mode: flm serve --asr 1
        args = {
            "serve",
            "--asr", "1",
            "--port", std::to_string(port_),
            "--host", "127.0.0.1",
            "--quiet"
        };
    } else if (model_type_ == ModelType::EMBEDDING) {
        // Embedding mode: flm serve --embed 1
        args = {
            "serve",
            "--embed", "1",
            "--port", std::to_string(port_),
            "--host", "127.0.0.1",
            "--quiet"
        };
    } else {
        // LLM mode (default): flm serve <checkpoint> --ctx-len N
        args = {
            "serve",
            model_info.checkpoint(),
            "--ctx-len", std::to_string(ctx_size),
            "--port", std::to_string(port_),
            "--host", "127.0.0.1",
            "--quiet"
        };
    }

    // Parse and append custom flm_args if provided
    if (!flm_args.empty()) {
        std::istringstream iss(flm_args);
        std::string token;
        while (iss >> token) {
            args.push_back(token);
        }
    }

    LOG(INFO, "FastFlowLM") << "Starting flm-server..." << std::endl;
    LOG(INFO, "ProcessManager") << "Starting process: \"" << flm_path << "\"";
    for (const auto& arg : args) {
        LOG(INFO, "ProcessManager") << " \"" << arg << "\"";
    }
    LOG(INFO, "ProcessManager") << std::endl;

    process_handle_ = utils::ProcessManager::start_process(flm_path, args, "", is_debug(), true);
    LOG(INFO, "ProcessManager") << "Process started successfully" << std::endl;

    // Wait for flm-server to be ready
    bool ready = wait_for_ready();
    if (!ready) {
        utils::ProcessManager::stop_process(process_handle_);
        process_handle_ = {nullptr, 0};  // Reset to prevent double-stop on destructor
        throw std::runtime_error("flm-server failed to start");
    }

    is_loaded_ = true;
    LOG(INFO, "FastFlowLM") << "Model loaded on port " << port_ << std::endl;
}

void FastFlowLMServer::unload() {
    LOG(INFO, "FastFlowLM") << "Unloading model..." << std::endl;
    if (is_loaded_ && process_handle_.pid != 0) {
        utils::ProcessManager::stop_process(process_handle_);
        process_handle_ = {nullptr, 0};
        port_ = 0;
        is_loaded_ = false;
    }
}

bool FastFlowLMServer::wait_for_ready() {
    // FLM doesn't have a health endpoint, so we use /api/tags to check if it's up
    std::string tags_url = get_base_url() + "/api/tags";

    LOG(INFO, "FastFlowLM") << "Waiting for " + server_name_ + " to be ready..." << std::endl;

    const int max_attempts = 300;  // 5 minutes timeout (large models can take time to load)
    for (int attempt = 0; attempt < max_attempts; ++attempt) {
        // Check if process is still running
        if (!utils::ProcessManager::is_running(process_handle_)) {
            LOG(ERROR, "FastFlowLM") << server_name_ << " process has terminated!" << std::endl;
            int exit_code = utils::ProcessManager::get_exit_code(process_handle_);
            LOG(ERROR, "FastFlowLM") << "Process exit code: " << exit_code << std::endl;
            LOG(ERROR, "FastFlowLM") << "Troubleshooting tips:" << std::endl;
            LOG(ERROR, "FastFlowLM") << "  1. Check if FLM is installed correctly: flm --version" << std::endl;
            LOG(ERROR, "FastFlowLM") << "  2. Try running: flm serve <model> --ctx-len 8192 --port 8001" << std::endl;
            LOG(ERROR, "FastFlowLM") << "  3. Check NPU drivers are installed (Windows only)" << std::endl;
            return false;
        }

        // Try to reach the /api/tags endpoint
        if (utils::HttpClient::is_reachable(tags_url, 1)) {
            LOG(INFO, "FastFlowLM") << server_name_ + " is ready!" << std::endl;
            return true;
        }

        // Sleep 1 second between attempts
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    LOG(ERROR, "FastFlowLM") << server_name_ << " failed to start within "
              << max_attempts << " seconds" << std::endl;
    return false;
}

json FastFlowLMServer::chat_completion(const json& request) {
    if (model_type_ == ModelType::AUDIO || model_type_ == ModelType::EMBEDDING) {
        return ErrorResponse::from_exception(
            UnsupportedOperationException("Chat completion", "FLM " + model_type_to_string(model_type_) + " model")
        );
    }

    // FLM requires the checkpoint name in the request (e.g., "gemma3:4b")
    // (whereas llama-server ignores the model name field)
    json modified_request = request;
    modified_request["model"] = checkpoint_;  // Use base class checkpoint field

    return forward_request("/v1/chat/completions", modified_request);
}

json FastFlowLMServer::completion(const json& request) {
    if (model_type_ == ModelType::AUDIO || model_type_ == ModelType::EMBEDDING) {
        return ErrorResponse::from_exception(
            UnsupportedOperationException("Text completion", "FLM " + model_type_to_string(model_type_) + " model")
        );
    }

    // FLM requires the checkpoint name in the request (e.g., "lfm2:1.2b")
    // (whereas llama-server ignores the model name field)
    json modified_request = request;
    modified_request["model"] = checkpoint_;  // Use base class checkpoint field

    return forward_request("/v1/completions", modified_request);
}

json FastFlowLMServer::embeddings(const json& request) {
    if (model_type_ == ModelType::AUDIO) {
        return ErrorResponse::from_exception(
            UnsupportedOperationException("Embeddings", "FLM " + model_type_to_string(model_type_) + " model")
        );
    }
    return forward_request("/v1/embeddings", request);
}

json FastFlowLMServer::reranking(const json& request) {
    if (model_type_ != ModelType::LLM) {
        return ErrorResponse::from_exception(
            UnsupportedOperationException("Reranking", "FLM " + model_type_to_string(model_type_) + " model")
        );
    }
    return forward_request("/v1/rerank", request);
}

json FastFlowLMServer::audio_transcriptions(const json& request) {
    if (model_type_ != ModelType::AUDIO) {
        return ErrorResponse::from_exception(
            UnsupportedOperationException("Audio transcription", "FLM " + model_type_to_string(model_type_) + " model")
        );
    }

    try {
        // Extract audio data from request (same format as WhisperServer)
        if (!request.contains("file_data")) {
            throw std::runtime_error("Missing 'file_data' in request");
        }

        std::string audio_data = request["file_data"].get<std::string>();
        std::string filename = request.value("filename", "audio.wav");

        // Determine content type from filename extension
        std::filesystem::path filepath(filename);
        std::string ext = filepath.extension().string();
        std::string content_type = "audio/wav";
        if (ext == ".mp3") content_type = "audio/mpeg";
        else if (ext == ".m4a") content_type = "audio/mp4";
        else if (ext == ".ogg") content_type = "audio/ogg";
        else if (ext == ".flac") content_type = "audio/flac";
        else if (ext == ".webm") content_type = "audio/webm";

        // Build multipart fields for FLM's /v1/audio/transcriptions endpoint
        std::vector<utils::MultipartField> fields;

        // Audio file field
        fields.push_back({
            "file",
            audio_data,
            filepath.filename().string(),
            content_type
        });

        // Model field (required by OpenAI API format)
        fields.push_back({"model", checkpoint_, "", ""});

        // Optional parameters
        if (request.contains("language")) {
            fields.push_back({"language", request["language"].get<std::string>(), "", ""});
        }
        if (request.contains("prompt")) {
            fields.push_back({"prompt", request["prompt"].get<std::string>(), "", ""});
        }
        if (request.contains("response_format")) {
            fields.push_back({"response_format", request["response_format"].get<std::string>(), "", ""});
        }
        if (request.contains("temperature")) {
            fields.push_back({"temperature", std::to_string(request["temperature"].get<double>()), "", ""});
        }

        return forward_multipart_request("/v1/audio/transcriptions", fields);

    } catch (const std::exception& e) {
        return json{
            {"error", {
                {"message", std::string("Transcription failed: ") + e.what()},
                {"type", "audio_processing_error"}
            }}
        };
    }
}

json FastFlowLMServer::responses(const json& request) {
    // Responses API is not supported for FLM backend
    return ErrorResponse::from_exception(
        UnsupportedOperationException("Responses API", "flm")
    );
}

void FastFlowLMServer::forward_streaming_request(const std::string& endpoint,
                                                  const std::string& request_body,
                                                  httplib::DataSink& sink,
                                                  bool sse,
                                                  long timeout_seconds) {
    // Streaming is only supported for LLM models
    if (model_type_ == ModelType::AUDIO || model_type_ == ModelType::EMBEDDING) {
        std::string error_msg = "data: {\"error\":{\"message\":\"Streaming not supported for FLM "
            + model_type_to_string(model_type_) + " model\",\"type\":\"unsupported_operation\"}}\n\n";
        sink.write(error_msg.c_str(), error_msg.size());
        return;
    }

    // FLM requires the checkpoint name in the model field (e.g., "gemma3:4b"),
    // not the Lemonade model name (e.g., "Gemma3-4b-it-FLM")
    try {
        json request = json::parse(request_body);
        request["model"] = checkpoint_;  // Use base class checkpoint field
        std::string modified_body = request.dump();

        // Call base class with modified request
        WrappedServer::forward_streaming_request(endpoint, modified_body, sink, sse, timeout_seconds);
    } catch (const json::exception& e) {
        // If JSON parsing fails, forward original request
        WrappedServer::forward_streaming_request(endpoint, request_body, sink, sse, timeout_seconds);
    }
}

std::string FastFlowLMServer::get_flm_path() {
#ifdef _WIN32
    // On Windows, use the standard install directory (auto-installed zip)
    try {
        std::string path = BackendUtils::get_backend_binary_path(SPEC, "npu");
        LOG(INFO, "FastFlowLM") << "Found flm at: " << path << std::endl;
        return path;
    } catch (const std::exception& e) {
        LOG(ERROR, "FastFlowLM") << "flm not found in install dir: " << e.what() << std::endl;
        return "";
    }
#else
    // On Linux, FLM is installed as a system package (in PATH)
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
