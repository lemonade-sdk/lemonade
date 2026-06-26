#include "lemon/backends/parakeet_server.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/audio_types.h"
#include "lemon/backend_manager.h"
#include "lemon/error_types.h"
#include "lemon/runtime_config.h"
#include "lemon/utils/custom_args.h"
#include "lemon/utils/http_client.h"
#include "lemon/utils/path_utils.h"
#include "lemon/utils/process_manager.h"
#include <chrono>
#include <filesystem>
#include <random>
#include <set>
#include <sstream>
#include <vector>
#include <lemon/utils/aixlog.hpp>

namespace fs = std::filesystem;
using namespace lemon::utils;

namespace lemon {
namespace backends {

ParakeetServer::ParakeetServer(const std::string& log_level, ModelManager* model_manager, BackendManager* backend_manager)
    : WrappedServer("parakeet-server", log_level, model_manager, backend_manager) {
    fs::path runtime_base = path_from_utf8(get_runtime_dir());

    std::random_device rd;
    std::uniform_int_distribution<unsigned int> dis(0, 0xFFFFFF);

    std::error_code ec;
    for (int attempt = 0; attempt < 8; ++attempt) {
        auto nonce = static_cast<unsigned long long>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        std::ostringstream suffix;
        suffix << "parakeet-audio-" << nonce << "-" << std::hex << dis(rd);
        fs::path candidate = runtime_base / suffix.str();

        ec.clear();
        if (fs::create_directory(candidate, ec)) {
            temp_dir_ = candidate;
            break;
        }
    }

    if (temp_dir_.empty()) {
        throw std::runtime_error("Failed to create temporary directory for ParakeetServer");
    }
}

ParakeetServer::~ParakeetServer() {
    unload();

    try {
        if (fs::exists(temp_dir_)) {
            fs::remove_all(temp_dir_);
        }
    } catch (const std::exception& e) {
        LOG(WARNING, "ParakeetServer") << "Could not clean up temp directory: "
                  << e.what() << std::endl;
    }
}

InstallParams ParakeetServer::get_install_params(const std::string& backend, const std::string& version) {
    InstallParams params;

    params.repo = "mudler/parakeet.cpp";

    if (backend == "cpu") {
#ifdef _WIN32
        params.filename = "parakeet-" + version + "-bin-win-cpu-x64.zip";
#elif defined(__linux__)
        params.filename = "parakeet-" + version + "-bin-linux-cpu-x64.tar.gz";
#elif defined(__APPLE__)
        params.filename = "parakeet-" + version + "-bin-macos-cpu-x64.tar.gz";
#else
        throw std::runtime_error("Unsupported platform for parakeet.cpp cpu backend");
#endif
    } else if (backend == "vulkan") {
#ifdef _WIN32
        params.filename = "parakeet-" + version + "-bin-win-vulkan-x64.zip";
#elif defined(__linux__)
        params.filename = "parakeet-" + version + "-bin-linux-vulkan-x64.tar.gz";
#else
        throw std::runtime_error("Vulkan parakeet.cpp backend is only supported on Windows and Linux");
#endif
    } else if (backend == "cuda") {
#ifdef _WIN32
        params.filename = "parakeet-" + version + "-bin-win-cuda-x64.zip";
#elif defined(__linux__)
        params.filename = "parakeet-" + version + "-bin-linux-cuda-x64.tar.gz";
#else
        throw std::runtime_error("CUDA parakeet.cpp backend is only supported on Windows and Linux");
#endif
    } else if (backend == "metal") {
#ifdef __APPLE__
        params.filename = "parakeet-" + version + "-bin-macos-metal-arm64.tar.gz";
#else
        throw std::runtime_error("Metal parakeet.cpp backend is only supported on macOS");
#endif
    } else {
        throw std::runtime_error("[ParakeetServer] Unknown parakeet backend: " + backend);
    }

    return params;
}

void ParakeetServer::load(const std::string& model_name,
                          const ModelInfo& model_info,
                          const RecipeOptions& options,
                          bool do_not_upgrade) {
    LOG(INFO, "ParakeetServer") << "Loading model: " << model_name << std::endl;
    LOG(INFO, "ParakeetServer") << "Per-model settings: " << options.to_log_string() << std::endl;

    std::string parakeetcpp_backend = options.get_option("parakeetcpp_backend");
    std::string parakeetcpp_args = options.get_option("parakeetcpp_args");

    RuntimeConfig::validate_backend_choice("parakeetcpp", parakeetcpp_backend);

    if (parakeetcpp_backend == "vulkan" || parakeetcpp_backend == "metal" || parakeetcpp_backend == "cuda") {
        device_type_ = DEVICE_GPU;
    } else {
        device_type_ = DEVICE_CPU;
    }

    backend_manager_->install_backend(SPEC.recipe, parakeetcpp_backend);

    std::string model_path = model_info.resolved_path();
    if (model_path.empty()) {
        throw std::runtime_error("Model file not found for checkpoint: " + model_info.checkpoint());
    }

    LOG(INFO, "ParakeetServer") << "Using model: " << model_path << std::endl;
    LOG(INFO, "ParakeetServer") << "Using backend: " << parakeetcpp_backend << std::endl;

    std::string exe_path = BackendUtils::get_backend_binary_path(SPEC, parakeetcpp_backend);

    port_ = choose_port();
    if (port_ == 0) {
        throw std::runtime_error("Failed to find an available port");
    }

    LOG(INFO, "ParakeetServer") << "Starting server on port " << port_ << std::endl;

    std::vector<std::string> args = {
        "--model", model_path,
        "--port", std::to_string(port_)
    };

    std::set<std::string> reserved_flags = {
        "--model",
        "--port"
    };

    if (!parakeetcpp_args.empty()) {
        std::string validation_error = validate_custom_args(parakeetcpp_args, reserved_flags);
        if (!validation_error.empty()) {
            throw std::invalid_argument(
                "Invalid custom parakeet-server arguments:\n" + validation_error
            );
        }

        LOG(DEBUG, "ParakeetServer") << "Adding custom arguments: " << parakeetcpp_args << std::endl;
        std::vector<std::string> custom_args_vec = parse_custom_args(parakeetcpp_args);
        args.insert(args.end(), custom_args_vec.begin(), custom_args_vec.end());
    }

    std::vector<std::pair<std::string, std::string>> env_vars;
    fs::path exe_dir = fs::path(exe_path).parent_path();

#ifndef _WIN32
    std::string lib_path = exe_dir.string();
    const char* existing_ld_path = std::getenv("LD_LIBRARY_PATH");
    if (existing_ld_path && strlen(existing_ld_path) > 0) {
        lib_path = lib_path + ":" + std::string(existing_ld_path);
    }
    env_vars.push_back({"LD_LIBRARY_PATH", lib_path});
#endif

    ProcessHandle started_handle = utils::ProcessManager::start_process(
        exe_path,
        args,
        "",
        is_debug(),
        false,
        env_vars
    );
    set_process_handle(started_handle);

    if (!has_process_handle(started_handle)) {
        throw std::runtime_error("Failed to start parakeet-server process");
    }

    LOG(INFO, "ParakeetServer") << "Process started with PID: " << started_handle.pid << std::endl;

    if (!wait_for_ready("/health")) {
        unload();
        throw std::runtime_error("parakeet-server failed to start or become ready");
    }

    LOG(INFO, "ParakeetServer") << "Server is ready!" << std::endl;
}

void ParakeetServer::unload() {
    stop_backend_watchdog();
    const ProcessHandle handle = consume_process_handle_for_cleanup();
    if (has_process_handle(handle)) {
        LOG(INFO, "ParakeetServer") << "Stopping server (PID: " << handle.pid << ")" << std::endl;
        utils::ProcessManager::stop_process(handle);
    }
}

json ParakeetServer::chat_completion(const json& request) {
    return json{
        {"error", {
            {"message", "Parakeet models do not support chat completion. Use audio transcription endpoints instead."},
            {"type", "unsupported_operation"},
            {"code", "model_not_applicable"}
        }}
    };
}

json ParakeetServer::completion(const json& request) {
    return json{
        {"error", {
            {"message", "Parakeet models do not support text completion. Use audio transcription endpoints instead."},
            {"type", "unsupported_operation"},
            {"code", "model_not_applicable"}
        }}
    };
}

json ParakeetServer::responses(const json& request) {
    return json{
        {"error", {
            {"message", "Parakeet models do not support responses. Use audio transcription endpoints instead."},
            {"type", "unsupported_operation"},
            {"code", "model_not_applicable"}
        }}
    };
}

json ParakeetServer::forward_multipart_audio_data(const std::string& audio_data,
                                                   const std::string& filename,
                                                   const json& params) {
    if (audio_data.empty()) {
        throw std::runtime_error("Empty audio data");
    }

    LOG(DEBUG, "ParakeetServer") << "Audio data size: " << audio_data.size() << " bytes" << std::endl;

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

    utils::MultipartField temp_field;
    temp_field.name = "temperature";
    temp_field.data = params.contains("temperature")
        ? std::to_string(params["temperature"].get<double>())
        : "0.0";
    fields.push_back(temp_field);

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

    const std::string url = "http://127.0.0.1:" + std::to_string(get_backend_port()) + "/v1/audio/transcriptions";
    LOG(DEBUG, "ParakeetServer") << "Sending multipart request to " << url << std::endl;

    auto res = utils::HttpClient::post_multipart(url, fields, 0);

    LOG(DEBUG, "ParakeetServer") << "Response status: " << res.status_code << std::endl;

    if (res.status_code != 200) {
        throw std::runtime_error("parakeet-server returned status " +
                                std::to_string(res.status_code) + ": " + res.body);
    }

    try {
        return json::parse(res.body);
    } catch (const json::parse_error&) {
        return json{{"text", res.body}};
    }
}

json ParakeetServer::audio_transcriptions(const json& request) {
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
