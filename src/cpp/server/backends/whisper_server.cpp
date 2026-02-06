#include "lemon/backends/whisper_server.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/audio_types.h"
#include "lemon/utils/http_client.h"
#include "lemon/utils/process_manager.h"
#include "lemon/utils/path_utils.h"
#include "lemon/utils/json_utils.h"
#include "lemon/error_types.h"
#include <httplib.h>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <vector>

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

WhisperServer::WhisperServer(const std::string& log_level, ModelManager* model_manager)
    : WrappedServer("whisper-server", log_level, model_manager) {

    // Create temp directory for audio files
    temp_dir_ = fs::temp_directory_path() / "lemonade_audio";
    fs::create_directories(temp_dir_);
}

WhisperServer::~WhisperServer() {
    unload();

    // Clean up temp directory
    try {
        if (fs::exists(temp_dir_)) {
            fs::remove_all(temp_dir_);
        }
    } catch (const std::exception& e) {
        std::cerr << "[WhisperServer] Warning: Could not clean up temp directory: "
                  << e.what() << std::endl;
    }
}

void WhisperServer::install(const std::string& backend) {
    std::string repo;
    std::string filename;
    std::string expected_version = BackendUtils::get_backend_version(SPEC.recipe, backend);

    // Determine download URL
    if (backend == "npu") {
        // NPU support from lemonade-sdk/whisper.cpp-npu
        repo = "lemonade-sdk/whisper.cpp-npu";

#ifdef _WIN32
        filename = "whisper-" + expected_version + "-windows-npu-x64.zip";
#else
        throw std::runtime_error("NPU whisper.cpp only supported on Windows");
#endif
        std::cout << "[WhisperServer] Using NPU backend" << std::endl;

    } else if (backend == "cpu") {
        // CPU-only builds from ggml-org/whisper.cpp
        repo = "ggml-org/whisper.cpp";

#ifdef _WIN32
        filename = "whisper-bin-x64.zip";
#elif defined(__linux__)
        filename = "whisper-bin-x64.zip";
#elif defined(__APPLE__)
        filename = "whisper-bin-arm64.zip";
#else
        throw std::runtime_error("Unsupported platform for whisper.cpp");
#endif
    } else {
        throw std::runtime_error("[WhisperServer] Unknown backend: " + backend);
    }

    BackendUtils::install_from_github(SPEC, expected_version, repo, filename, backend);
}

// Helper to determine NPU compiled cache info based on model info from server_models.json
static std::pair<std::string, std::string> get_npu_cache_info(const ModelInfo& model_info) {

    if (!model_info.npu_cache_repo.empty() && !model_info.npu_cache_filename.empty()) {
        std::cout << "[WhisperServer] Using NPU cache from server_models.json: "
                  << model_info.npu_cache_repo << " / " << model_info.npu_cache_filename << std::endl;
        return {model_info.npu_cache_repo, model_info.npu_cache_filename};
    }

    // No NPU cache configured for this model in server_models.json
    std::cout << "[WhisperServer] No NPU cache configured for model: " << model_info.model_name << std::endl;
    return {"", ""};
}

// Helper to download NPU compiled cache (.rai file) for a given ggml .bin model
void WhisperServer::download_npu_compiled_cache(const std::string& model_path,
                                                const std::string& checkpoint,
                                                const ModelInfo& model_info,
                                                bool do_not_upgrade) {
    auto [cache_repo, cache_filename] = get_npu_cache_info(model_info);

    if (cache_repo.empty() || cache_filename.empty()) {
        std::cout << "[WhisperServer] No NPU compiled cache available for this model" << std::endl;
        return;
    }

    std::cout << "[WhisperServer] Downloading NPU compiled cache: " << cache_filename << std::endl;
    std::cout << "[WhisperServer] From repository: " << cache_repo << std::endl;

    // Determine where to place the .rai file (must be in the same directory as .bin file)
    fs::path model_dir = fs::path(model_path).parent_path();
    fs::path cache_path = model_dir / cache_filename;

    // Check if cache already exists
    if (fs::exists(cache_path) && !do_not_upgrade) {
        std::cout << "[WhisperServer] NPU cache already exists: " << cache_path << std::endl;
        return;
    }

    try {
        // Download .rai file directly from HuggingFace using HttpClient
        std::string hf_url = "https://huggingface.co/" + cache_repo + "/resolve/main/" + cache_filename;

        std::cout << "[WhisperServer] Downloading from: " << hf_url << std::endl;

        // Download directly to the target location
        auto download_result = utils::HttpClient::download_file(
            hf_url,
            cache_path.string(),
            utils::create_throttled_progress_callback()
        );

        if (!download_result.success) {
            throw std::runtime_error("Failed to download NPU cache from: " + hf_url + " - " + download_result.error_message);
        }

        std::cout << "[WhisperServer] NPU cache ready at: " << cache_path << std::endl;

    } catch (const std::exception& e) {
        if (fs::exists(cache_path)) {
            fs::remove(cache_path);
            std::cout << "[WhisperServer] Cleaned up partial cache file" << std::endl;
        }

        std::cerr << "[WhisperServer] Warning: Failed to download NPU cache: " << e.what() << std::endl;
        std::cerr << "[WhisperServer] Continuing without NPU cache (may cause runtime errors)" << std::endl;
    }
}

std::string WhisperServer::download_model(const std::string& checkpoint,
                                         const std::string& mmproj,
                                         bool do_not_upgrade) {
    std::string repo, filename;
    size_t colon_pos = checkpoint.find(':');

    if (colon_pos != std::string::npos) {
        repo = checkpoint.substr(0, colon_pos);
        filename = checkpoint.substr(colon_pos + 1);
    } else {
        throw std::runtime_error("Invalid checkpoint format. Expected 'repo:filename'");
    }

    if (!model_manager_) {
        throw std::runtime_error("ModelManager not available for model download");
    }

    std::cout << "[WhisperServer] Downloading model: " << filename << " from " << repo << std::endl;

    model_manager_->download_model(
        checkpoint,  // model_name
        checkpoint,  // checkpoint
        "whispercpp",  // recipe
        false,  // reasoning
        false,  // vision
        false,  // embedding
        false,  // reranking
        false,  // image
        "",     // mmproj
        do_not_upgrade
    );

    ModelInfo info = model_manager_->get_model_info(checkpoint);
    std::string model_path = info.resolved_path;

    if (model_path.empty() || !fs::exists(model_path)) {
        throw std::runtime_error("Failed to download Whisper model: " + checkpoint);
    }

    std::cout << "[WhisperServer] Model downloaded to: " << model_path << std::endl;
    return model_path;
}

void WhisperServer::load(const std::string& model_name,
                        const ModelInfo& model_info,
                        const RecipeOptions& options,
                        bool do_not_upgrade) {
    std::cout << "[WhisperServer] Loading model: " << model_name << std::endl;
    std::cout << "[WhisperServer] Per-model settings: " << options.to_log_string() << std::endl;

    std::string whispercpp_backend = options.get_option("whispercpp_backend");

    install(whispercpp_backend);

    std::string model_path = model_info.resolved_path;
    if (model_path.empty()) {
        throw std::runtime_error("Model file not found for checkpoint: " + model_info.checkpoint);
    }

    std::cout << "[WhisperServer] Using model: " << model_path << std::endl;
    std::cout << "[WhisperServer] Using backend: " << whispercpp_backend << std::endl;
    model_path_ = model_path;

    // For NPU backend, download the compiled cache (.rai file). This is a must-have for NPU backend.
    if (whispercpp_backend == "npu") {
        download_npu_compiled_cache(model_path, model_info.checkpoint, model_info, do_not_upgrade);
    }

    // Get whisper-server executable path
    std::string exe_path = BackendUtils::get_backend_binary_path(SPEC, whispercpp_backend);

    // Choose a port
    port_ = choose_port();
    if (port_ == 0) {
        throw std::runtime_error("Failed to find an available port");
    }

    std::cout << "[WhisperServer] Starting server on port " << port_ << std::endl;

    // Build command line arguments
    // Note: whisper.cpp server handles audio conversion automatically since v1.8
    // Note: Don't include exe_path here - ProcessManager::start_process already handles it
    std::vector<std::string> args = {
        "-m", model_path_,
        "--port", std::to_string(port_)
    };

    // Note: whisper-server doesn't support --debug flag

    // Launch the subprocess
    process_handle_ = utils::ProcessManager::start_process(
        exe_path,
        args,
        "",     // working_dir (empty = current)
        is_debug()  // inherit_output
    );

    if (process_handle_.pid == 0) {
        throw std::runtime_error("Failed to start whisper-server process");
    }

    std::cout << "[WhisperServer] Process started with PID: " << process_handle_.pid << std::endl;

    // Wait for server to be ready
    if (!wait_for_ready("/health")) {
        unload();
        throw std::runtime_error("whisper-server failed to start or become ready");
    }

    std::cout << "[WhisperServer] Server is ready!" << std::endl;
}

void WhisperServer::unload() {
    if (process_handle_.pid != 0) {
        std::cout << "[WhisperServer] Stopping server (PID: " << process_handle_.pid << ")" << std::endl;
        utils::ProcessManager::stop_process(process_handle_);
        process_handle_ = {nullptr, 0};
        port_ = 0;
        model_path_.clear();
    }
}

// ICompletionServer implementation - not supported for Whisper
json WhisperServer::chat_completion(const json& request) {
    return json{
        {"error", {
            {"message", "Whisper models do not support chat completion. Use audio transcription endpoints instead."},
            {"type", "unsupported_operation"},
            {"code", "model_not_applicable"}
        }}
    };
}

json WhisperServer::completion(const json& request) {
    return json{
        {"error", {
            {"message", "Whisper models do not support text completion. Use audio transcription endpoints instead."},
            {"type", "unsupported_operation"},
            {"code", "model_not_applicable"}
        }}
    };
}

json WhisperServer::responses(const json& request) {
    return json{
        {"error", {
            {"message", "Whisper models do not support responses. Use audio transcription endpoints instead."},
            {"type", "unsupported_operation"},
            {"code", "model_not_applicable"}
        }}
    };
}

// Audio file handling helpers
std::string WhisperServer::save_audio_to_temp(const std::string& audio_data,
                                              const std::string& filename) {
    // Generate unique filename
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 999999);

    std::string ext = fs::path(filename).extension().string();
    if (ext.empty()) {
        ext = ".audio";  // Default extension
    }

    std::stringstream ss;
    ss << "audio_" << std::setfill('0') << std::setw(6) << dis(gen) << ext;

    fs::path temp_file = temp_dir_ / ss.str();

    // Write audio data to file
    std::ofstream outfile(temp_file, std::ios::binary);
    if (!outfile) {
        throw std::runtime_error("Failed to create temporary audio file: " + temp_file.string());
    }

    outfile.write(audio_data.data(), audio_data.size());
    outfile.close();

    if (is_debug()) {
        std::cout << "[WhisperServer] Saved audio to temp file: " << temp_file << std::endl;
    }

    return temp_file.string();
}

void WhisperServer::cleanup_temp_file(const std::string& path) {
    try {
        if (fs::exists(path)) {
            fs::remove(path);
            if (is_debug()) {
                std::cout << "[WhisperServer] Cleaned up temp file: " << path << std::endl;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[WhisperServer] Warning: Could not delete temp file " << path
                  << ": " << e.what() << std::endl;
    }
}

void WhisperServer::validate_audio_file(const std::string& path) {
    if (!fs::exists(path)) {
        throw std::runtime_error("Audio file does not exist: " + path);
    }

    std::uintmax_t file_size = fs::file_size(path);
    if (file_size > audio::Limits::MAX_FILE_SIZE_BYTES) {
        throw std::runtime_error("Audio file exceeds maximum size of 25MB");
    }

    if (file_size == 0) {
        throw std::runtime_error("Audio file is empty");
    }
}

json WhisperServer::build_transcription_request(const json& request, bool translate) {
    json whisper_req;

    // Required fields
    if (request.contains("file_path")) {
        whisper_req["file"] = request["file_path"];
    }

    // Optional fields
    if (request.contains("language") && !translate) {
        // For transcription, respect language hint
        whisper_req["language"] = request["language"];
    }
    // For translation, don't specify language (always translates to English)

    if (request.contains("prompt")) {
        whisper_req["prompt"] = request["prompt"];
    }

    if (request.contains("temperature")) {
        whisper_req["temperature"] = request["temperature"];
    }

    if (request.contains("response_format")) {
        whisper_req["response_format"] = request["response_format"];
    } else {
        whisper_req["response_format"] = "json";  // Default
    }

    // Add translate flag if needed
    if (translate) {
        whisper_req["translate"] = true;
    }

    return whisper_req;
}

// Forward audio file to whisper-server using multipart form-data
json WhisperServer::forward_multipart_audio_request(const std::string& file_path,
                                                    const json& params,
                                                    bool translate) {
    // Read the audio file content
    std::ifstream file(file_path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Could not open audio file: " + file_path);
    }

    std::ostringstream oss;
    oss << file.rdbuf();
    std::string file_content = oss.str();
    file.close();

    if (is_debug()) {
        std::cout << "[WhisperServer] Audio file size: " << file_content.size() << " bytes" << std::endl;
    }

    // Determine content type based on file extension
    fs::path filepath(file_path);
    std::string ext = filepath.extension().string();
    std::string content_type = "audio/wav";  // Default

    // Map common audio extensions to MIME types
    if (ext == ".mp3") content_type = "audio/mpeg";
    else if (ext == ".wav") content_type = "audio/wav";
    else if (ext == ".m4a") content_type = "audio/mp4";
    else if (ext == ".ogg") content_type = "audio/ogg";
    else if (ext == ".flac") content_type = "audio/flac";
    else if (ext == ".webm") content_type = "audio/webm";

    // Build multipart form data using httplib::UploadFormDataItems
    httplib::UploadFormDataItems items;

    // Add the audio file
    httplib::UploadFormData audio_file;
    audio_file.name = "file";
    audio_file.content = file_content;
    audio_file.filename = filepath.filename().string();
    audio_file.content_type = content_type;
    items.push_back(audio_file);

    // Add optional parameters as form fields
    std::string response_format = params.value("response_format", "json");
    httplib::UploadFormData fmt_field;
    fmt_field.name = "response_format";
    fmt_field.content = response_format;
    items.push_back(fmt_field);

    httplib::UploadFormData temp_field;
    temp_field.name = "temperature";
    if (params.contains("temperature")) {
        temp_field.content = std::to_string(params["temperature"].get<double>());
    } else {
        temp_field.content = "0.0";
    }
    items.push_back(temp_field);

    if (params.contains("language")) {
        httplib::UploadFormData lang_field;
        lang_field.name = "language";
        lang_field.content = params["language"].get<std::string>();
        items.push_back(lang_field);
    }

    if (params.contains("prompt")) {
        httplib::UploadFormData prompt_field;
        prompt_field.name = "prompt";
        prompt_field.content = params["prompt"].get<std::string>();
        items.push_back(prompt_field);
    }

    if (translate) {
        httplib::UploadFormData translate_field;
        translate_field.name = "translate";
        translate_field.content = "true";
        items.push_back(translate_field);
    }

    // Create httplib client
    httplib::Client cli("127.0.0.1", port_);
    cli.set_connection_timeout(30);  // 30 second connection timeout
    cli.set_read_timeout(300);       // 5 minute read timeout for transcription

    if (is_debug()) {
        std::cout << "[WhisperServer] Sending multipart request to http://127.0.0.1:"
                  << port_ << "/inference" << std::endl;
    }

    // Send the multipart POST request
    httplib::Result res = cli.Post("/inference", items);

    if (!res) {
        httplib::Error err = res.error();
        throw std::runtime_error("HTTP request failed: " + httplib::to_string(err));
    }

    if (is_debug()) {
        std::cout << "[WhisperServer] Response status: " << res->status << std::endl;
        std::cout << "[WhisperServer] Response body: " << res->body << std::endl;
    }

    // Parse response
    if (res->status != 200) {
        throw std::runtime_error("whisper-server returned status " +
                                std::to_string(res->status) + ": " + res->body);
    }

    // Try to parse as JSON
    try {
        return json::parse(res->body);
    } catch (const json::parse_error&) {
        // If response_format is not json, return it wrapped
        return json{{"text", res->body}};
    }
}

// IAudioServer implementation
json WhisperServer::audio_transcriptions(const json& request) {
    try {
        // Extract audio data from request
        if (!request.contains("file_data")) {
            throw std::runtime_error("Missing 'file_data' in request");
        }

        std::string audio_data = request["file_data"].get<std::string>();
        std::string filename = request.value("filename", "audio.audio");

        // Save to temporary file
        std::string temp_file = save_audio_to_temp(audio_data, filename);

        // Validate the file
        validate_audio_file(temp_file);

        // Forward to whisper-server using multipart form-data
        json result = forward_multipart_audio_request(temp_file, request, false);

        // Clean up temp file
        cleanup_temp_file(temp_file);

        return result;

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
