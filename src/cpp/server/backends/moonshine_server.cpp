#include "lemon/backends/moonshine_server.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backend_manager.h"
#include "lemon/runtime_config.h"
#include "lemon/utils/custom_args.h"
#include "lemon/utils/http_client.h"
#include "lemon/utils/process_manager.h"
#include "lemon/error_types.h"
#include <iostream>
#include <filesystem>
#include <set>
#include <vector>
#include <lemon/utils/aixlog.hpp>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace lemon::utils;

namespace lemon {
namespace backends {

InstallParams MoonshineServer::get_install_params(const std::string& backend, const std::string& version) {
    (void)backend;  // moonshine is CPU-only
    InstallParams params;
    params.repo = "lemonade-sdk/moonshine-server-rocm";

    // Self-contained PyInstaller bundles built by the lemonade-sdk/moonshine-server-rocm
    // distribution repo (tracks moonshine-voice PyPI releases; tag scheme
    // moonshine<version>) — no system Python needed. moonshine-voice publishes
    // wheels for win x64, linux x64/arm64, and macOS arm64 only (no Intel
    // macOS), so the macOS asset is arm64-only.
#ifdef _WIN32
    params.filename = "moonshine-server-" + version + "-windows-x64.zip";
#elif defined(__APPLE__)
    params.filename = "moonshine-server-" + version + "-macos-arm64.tar.gz";
#else
    params.filename = "moonshine-server-" + version + "-linux-x64.tar.gz";
#endif

    return params;
}

MoonshineServer::MoonshineServer(const std::string& log_level, ModelManager* model_manager,
                                 BackendManager* backend_manager)
    : WrappedServer("moonshine-server", log_level, model_manager, backend_manager) {
}

MoonshineServer::~MoonshineServer() {
    unload();
}

void MoonshineServer::load(const std::string& model_name,
                          const ModelInfo& model_info,
                          const RecipeOptions& options,
                          bool do_not_upgrade) {
    (void)do_not_upgrade;
    LOG(INFO, "MoonshineServer") << "Loading model: " << model_name << std::endl;
    LOG(INFO, "MoonshineServer") << "Per-model settings: " << options.to_log_string() << std::endl;

    std::string moonshine_args = options.get_option("moonshine_args");

    device_type_ = DEVICE_CPU;

    // Install moonshine-server if needed
    backend_manager_->install_backend(SPEC.recipe, "cpu");

    // Resolve model path from ModelManager (standard HF cache)
    std::string model_path = model_info.resolved_path();
    if (model_path.empty() || !fs::exists(model_path)) {
        throw std::runtime_error("Model directory not found for checkpoint: " + model_info.checkpoint());
    }

    LOG(INFO, "MoonshineServer") << "Using model: " << model_path << std::endl;

    // Resolve model architecture. Prefer the explicit registry field; fall back
    // to inferring from the checkpoint variant (onnx/tiny, onnx/small, etc.).
    int model_arch = model_info.moonshine_arch;
    if (model_arch < 0) {
        std::string variant = model_info.checkpoint();
        std::transform(variant.begin(), variant.end(), variant.begin(), ::tolower);
        if (variant.find("tiny") != std::string::npos) {
            model_arch = 2;
        } else if (variant.find("small") != std::string::npos) {
            model_arch = 4;
        } else {
            model_arch = 5; // MEDIUM_STREAMING
        }
    }

    // Get executable path
    std::string executable = BackendUtils::get_backend_binary_path(SPEC, "cpu");
    LOG(INFO, "MoonshineServer") << "Using executable: " << executable << std::endl;

    // moonshine-server binds three consecutive ports: HTTP, WS (+1), TCP (+2).
    // Probe all three before launching instead of assuming +1/+2 are free.
    port_ = 0;
    tcp_port_ = 0;
    int start = 8001;
    for (int attempt = 0; attempt < 50; ++attempt) {
        int p = utils::ProcessManager::find_free_port(start);
        if (p == 0) {
            break;
        }
        if (utils::ProcessManager::find_free_port(p + 1) == p + 1 &&
            utils::ProcessManager::find_free_port(p + 2) == p + 2) {
            port_ = p;
            tcp_port_ = p + 2;
            break;
        }
        start = p + 1;
    }
    if (port_ == 0 || tcp_port_ == 0) {
        throw std::runtime_error("Failed to find three consecutive available ports");
    }

    LOG(INFO, "MoonshineServer") << "Starting server on port " << port_
                                 << " (TCP streaming on " << tcp_port_ << ")" << std::endl;

    // Note: Don't include exe_path here - ProcessManager::start_process already handles it
    std::vector<std::string> args = {
        "--model-path", model_path,
        "--model-arch", std::to_string(model_arch),
        "--port", std::to_string(port_),
        "--tcp-port", std::to_string(tcp_port_)
    };

    // Lemonade manages the model path and ports; optional moonshine-server
    // flags come from moonshine_args.
    std::set<std::string> reserved_flags = {
        "--model-path",
        "--model-arch",
        "--port",
        "--tcp-port"
    };

    if (!moonshine_args.empty()) {
        std::string validation_error = validate_custom_args(moonshine_args, reserved_flags);
        if (!validation_error.empty()) {
            throw std::invalid_argument(
                "Invalid custom moonshine-server arguments:\n" + validation_error
            );
        }

        LOG(DEBUG, "MoonshineServer") << "Adding custom arguments: " << moonshine_args << std::endl;
        std::vector<std::string> custom_args_vec = parse_custom_args(moonshine_args);
        args.insert(args.end(), custom_args_vec.begin(), custom_args_vec.end());
    }

    // Set environment variables
    std::vector<std::pair<std::string, std::string>> env_vars;
    // Prevent system/user Python packages from leaking into the bundled environment
    env_vars.push_back({"PYTHONNOUSERSITE", "1"});

    // Launch the subprocess
    bool inherit_output = (log_level_ == "info") || is_debug();
    process_handle_ = utils::ProcessManager::start_process(
        executable,
        args,
        "",     // working_dir
        inherit_output,
        false,  // filter_health_logs
        env_vars
    );

    if (process_handle_.pid == 0) {
        throw std::runtime_error("Failed to start moonshine-server process");
    }

    LOG(INFO, "MoonshineServer") << "Process started with PID: " << process_handle_.pid << std::endl;

    // Wait for server to be ready
    if (!wait_for_ready("/health")) {
        unload();
        throw std::runtime_error("moonshine-server failed to start or become ready");
    }

    LOG(INFO, "MoonshineServer") << "Server is ready!" << std::endl;
}

void MoonshineServer::unload() {
    if (process_handle_.pid != 0) {
        LOG(INFO, "MoonshineServer") << "Stopping server (PID: " << process_handle_.pid << ")" << std::endl;
        utils::ProcessManager::stop_process(process_handle_);
        process_handle_ = {nullptr, 0};
        port_ = 0;
        tcp_port_ = 0;
    }
}

std::string MoonshineServer::get_streaming_address() {
    if (tcp_port_ == 0) {
        return "";  // not loaded
    }
    return "tcp://127.0.0.1:" + std::to_string(tcp_port_);
}

// ICompletionServer implementation - not supported for Moonshine
json MoonshineServer::chat_completion(const json& request) {
    (void)request;
    return json{
        {"error", {
            {"message", "Moonshine models do not support chat completion. Use audio transcription endpoints instead."},
            {"type", "unsupported_operation"},
            {"code", "model_not_applicable"}
        }}
    };
}

json MoonshineServer::completion(const json& request) {
    (void)request;
    return json{
        {"error", {
            {"message", "Moonshine models do not support text completion. Use audio transcription endpoints instead."},
            {"type", "unsupported_operation"},
            {"code", "model_not_applicable"}
        }}
    };
}

json MoonshineServer::responses(const json& request) {
    (void)request;
    return json{
        {"error", {
            {"message", "Moonshine models do not support responses. Use audio transcription endpoints instead."},
            {"type", "unsupported_operation"},
            {"code", "model_not_applicable"}
        }}
    };
}

json MoonshineServer::forward_multipart_audio_data(const std::string& audio_data,
                                                   const std::string& filename,
                                                   const json& params) {
    if (audio_data.empty()) {
        throw std::runtime_error("Empty audio data");
    }

    LOG(DEBUG, "MoonshineServer") << "Audio data size: " << audio_data.size() << " bytes" << std::endl;

    fs::path filepath(filename);
    std::string ext = filepath.extension().string();
    std::string content_type = "audio/wav";

    if (ext == ".mp3") content_type = "audio/mpeg";
    else if (ext == ".wav") content_type = "audio/wav";
    else if (ext == ".m4a") content_type = "audio/mp4";
    else if (ext == ".ogg") content_type = "audio/ogg";
    else if (ext == ".flac") content_type = "audio/flac";
    else if (ext == ".webm") content_type = "audio/webm";

    std::vector<utils::MultipartField> fields;

    utils::MultipartField audio_file;
    audio_file.name = "file";
    audio_file.data = audio_data;
    audio_file.filename = filepath.filename().string();
    audio_file.content_type = content_type;
    fields.push_back(audio_file);

    std::string response_format = params.value("response_format", "json");
    utils::MultipartField fmt_field;
    fmt_field.name = "response_format";
    fmt_field.data = response_format;
    fields.push_back(fmt_field);

    if (params.contains("language")) {
        utils::MultipartField lang_field;
        lang_field.name = "language";
        lang_field.data = params["language"].get<std::string>();
        fields.push_back(lang_field);
    }

    if (params.contains("prompt")) {
        utils::MultipartField prompt_field;
        prompt_field.name = "prompt";
        prompt_field.data = params["prompt"].get<std::string>();
        fields.push_back(prompt_field);
    }

    const std::string url = "http://127.0.0.1:" + std::to_string(port_) + "/inference";
    LOG(DEBUG, "MoonshineServer") << "Sending multipart request to " << url << " (direct data)" << std::endl;

    auto res = utils::HttpClient::post_multipart(url, fields, 0);

    LOG(DEBUG, "MoonshineServer") << "Response status: " << res.status_code << std::endl;

    if (res.status_code != 200) {
        throw std::runtime_error("moonshine-server returned status " +
                                std::to_string(res.status_code) + ": " + res.body);
    }

    try {
        return json::parse(res.body);
    } catch (const json::parse_error&) {
        return json{{"text", res.body}};
    }
}

// ITranscriptionServer implementation
json MoonshineServer::audio_transcriptions(const json& request) {
    try {
        if (!request.contains("file_data")) {
            throw std::runtime_error("Missing 'file_data' in request");
        }

        std::string audio_data = request["file_data"].get<std::string>();
        std::string filename = request.value("filename", "audio.wav");

        return forward_multipart_audio_data(audio_data, filename, request);

    } catch (const std::exception& e) {
        return json{
            {"error", {
                {"message", std::string("Transcription failed: ") + e.what()},
                {"type", "audio_processing_error"}
            }}
        };
    }
}

} // namespace backends
} // namespace lemon
