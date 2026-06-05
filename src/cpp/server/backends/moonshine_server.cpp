#include "lemon/backends/moonshine_server.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backend_manager.h"
#include "lemon/runtime_config.h"
#include "lemon/audio_types.h"
#include "lemon/utils/custom_args.h"
#include "lemon/utils/http_client.h"
#include "lemon/utils/path_utils.h"
#include "lemon/utils/process_manager.h"
#include "lemon/error_types.h"
#include <iostream>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
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

// Moonshine-server script is bundled with Lemonade source.
// The moonshine_voice Python package is a runtime dependency (install via pip).
InstallParams MoonshineServer::get_install_params(const std::string& backend, const std::string& version) {
    InstallParams params;
    // Empty repo/filename signals that auto-install is not required via GitHub releases.
    // moonshine_voice must be installed separately: pip install moonshine_voice
    return params;
}

namespace {

// Check if moonshine_voice is importable in the Python environment.
bool is_moonshine_voice_available() {
    std::string output;
    int exit_code = ProcessManager::run_command(
        "python3 -c \"import moonshine_voice; print('ok')\"",
        output, 10);
    return exit_code == 0 && output.find("ok") != std::string::npos;
}

// Search for the moonshine-server Python script in common locations.
std::string find_moonshine_server_script() {
    // 1. Environment override
    const char* env_path = std::getenv("LEMONADE_MOONSHINE_SERVER");
    if (env_path && fs::exists(env_path)) {
        return std::string(env_path);
    }

    // 2. Relative to current working directory (development layout)
    std::vector<std::string> candidates = {
        "tools/moonshine-server/main.py",
        "../tools/moonshine-server/main.py",
        "../../tools/moonshine-server/main.py",
        "../../../tools/moonshine-server/main.py",
    };

    for (const auto& candidate : candidates) {
        if (fs::exists(candidate)) {
            return fs::absolute(candidate).string();
        }
    }

    // 3. Check in the downloaded backends directory
    std::string install_dir = BackendUtils::get_install_directory("moonshine", "");
    std::string installed = BackendUtils::find_executable_in_install_dir(install_dir, "moonshine-server.py");
    if (!installed.empty()) {
        return installed;
    }

    return "";
}

// Resolve or auto-download a Moonshine model path using the installed moonshine_voice package.
// Returns empty string if resolution fails.
std::string resolve_moonshine_model_path(int model_arch) {
    // Build a Python one-liner that resolves the model path via moonshine_voice.download
    std::ostringstream py_cmd;
    py_cmd << "from moonshine_voice.download import find_model_info, download_model_from_info; "
           << "from moonshine_voice.moonshine_api import ModelArch; "
           << "info = find_model_info('en', ModelArch(" << model_arch << ")); "
           << "path, _ = download_model_from_info(info); "
           << "print(path, end='')";

    std::string output;
    std::string cmd = "python3 -c \"" + py_cmd.str() + "\"";
    int exit_code = ProcessManager::run_command(cmd, output, 60);
    if (exit_code != 0) {
        LOG(WARNING, "MoonshineServer") << "Failed to resolve Moonshine model path: " << output << std::endl;
        return "";
    }
    // Trim whitespace
    size_t end = output.find_last_not_of(" \t\n\r");
    if (end != std::string::npos) {
        output = output.substr(0, end + 1);
    }
    return output;
}

} // anonymous namespace

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

    device_type_ = DEVICE_CPU;

    // Find the Python script first (needed for model path fallback)
    std::string script_path = find_moonshine_server_script();
    if (script_path.empty()) {
        throw std::runtime_error(
            "moonshine-server.py not found. Set LEMONADE_MOONSHINE_SERVER env var "
            "or ensure tools/moonshine-server/main.py exists relative to the working directory.");
    }

    // Resolve model architecture from recipe options or default to MEDIUM_STREAMING (5)
    json arch_json = options.get_option("moonshine_arch");
    std::string arch_str = arch_json.is_string() ? arch_json.get<std::string>() : "";
    if (!arch_str.empty()) {
        try {
            model_arch_ = std::stoi(arch_str);
        } catch (const std::exception&) {
            LOG(WARNING, "MoonshineServer") << "Invalid moonshine_arch value: " << arch_str
                      << ", using default 5 (MEDIUM_STREAMING)" << std::endl;
            model_arch_ = 5;
        }
    } else {
        model_arch_ = 5; // MEDIUM_STREAMING
    }

    std::string model_path = model_info.resolved_path();
    if (model_path.empty() || !fs::exists(model_path)) {
        LOG(INFO, "MoonshineServer") << "Model path not resolved from ModelManager, attempting auto-resolve..." << std::endl;
        model_path = resolve_moonshine_model_path(model_arch_);
        if (model_path.empty()) {
            throw std::runtime_error("Model directory not found for checkpoint: " + model_info.checkpoint());
        }
    }

    LOG(INFO, "MoonshineServer") << "Using model: " << model_path << std::endl;

    LOG(INFO, "MoonshineServer") << "Using script: " << script_path << std::endl;

    // Choose ports for HTTP and TCP streaming
    port_ = choose_port();
    if (port_ == 0) {
        throw std::runtime_error("Failed to find an available port");
    }

    tcp_port_ = port_ + 2;  // HTTP + 2 (WS is +1)
    // Note: We assume port_+2 is available since choose_port() found a free port
    // and our port range is large enough. If collisions occur, find_free_port
    // can be used here too.

    LOG(INFO, "MoonshineServer") << "Starting server on port " << port_
                                 << " (TCP streaming on " << tcp_port_ << ")" << std::endl;

    // Verify moonshine_voice is installed before starting the subprocess
    if (!is_moonshine_voice_available()) {
        throw std::runtime_error(
            "moonshine_voice Python package is not installed. "
            "Install it with: pip install moonshine_voice");
    }

    std::vector<std::pair<std::string, std::string>> env_vars;

    std::vector<std::string> args = {
        script_path,
        "--model-path", model_path,
        "--model-arch", std::to_string(model_arch_),
        "--port", std::to_string(port_),
        "--tcp-port", std::to_string(tcp_port_)
    };

    std::string python_exe = "python3";
#ifdef _WIN32
    python_exe = "python.exe";
#endif

    // Launch the subprocess
    process_handle_ = utils::ProcessManager::start_process(
        python_exe,
        args,
        "",     // working_dir
        is_debug(),  // inherit_output
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

json MoonshineServer::build_transcription_request(const json& request) {
    json moonshine_req;

    if (request.contains("file_path")) {
        moonshine_req["file"] = request["file_path"];
    }

    if (request.contains("language")) {
        moonshine_req["language"] = request["language"];
    }

    if (request.contains("prompt")) {
        moonshine_req["prompt"] = request["prompt"];
    }

    if (request.contains("temperature")) {
        moonshine_req["temperature"] = request["temperature"];
    }

    if (request.contains("response_format")) {
        moonshine_req["response_format"] = request["response_format"];
    } else {
        moonshine_req["response_format"] = "json";
    }

    return moonshine_req;
}

json MoonshineServer::forward_multipart_audio_request(const std::string& file_path,
                                                      const json& params) {
    std::ifstream file(file_path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Could not open audio file: " + file_path);
    }

    std::ostringstream oss;
    oss << file.rdbuf();
    std::string file_content = oss.str();
    file.close();

    LOG(DEBUG, "MoonshineServer") << "Audio file size: " << file_content.size() << " bytes" << std::endl;

    fs::path filepath(file_path);
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
    audio_file.data = file_content;
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
    LOG(DEBUG, "MoonshineServer") << "Sending multipart request to " << url << std::endl;

    auto res = utils::HttpClient::post_multipart(url, fields, 0);

    LOG(DEBUG, "MoonshineServer") << "Response status: " << res.status_code << std::endl;
    LOG(DEBUG, "MoonshineServer") << "Response body: " << res.body << std::endl;

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
