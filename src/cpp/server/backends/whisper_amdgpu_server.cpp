#include "lemon/backends/whisper_amdgpu_server.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backend_manager.h"
#include "lemon/utils/process_manager.h"
#include "lemon/utils/http_client.h"
#include "lemon/error_types.h"
#include <filesystem>
#include <lemon/utils/aixlog.hpp>

namespace fs = std::filesystem;
using namespace lemon::utils;

namespace lemon {
namespace backends {

InstallParams WhisperAMDGPUServer::get_install_params(const std::string& /*backend*/, const std::string& /*version*/) {
    // No GitHub release: the venv + scripts are installed locally into the cache
    // (bin/amdgpu-whisper/gpu/). Values here are only consulted if a download is
    // triggered, which does not happen for this local backend.
    return {"lemonade-sdk/amdgpu-whisper", "amdgpu-whisper.zip"};
}

WhisperAMDGPUServer::WhisperAMDGPUServer(const std::string& log_level,
                                         ModelManager* model_manager,
                                         BackendManager* backend_manager)
    : WrappedServer("AMDGPU-Whisper", log_level, model_manager, backend_manager) {
}

WhisperAMDGPUServer::~WhisperAMDGPUServer() {
    if (is_loaded_) {
        try {
            unload();
        } catch (...) {
        }
    }
}

void WhisperAMDGPUServer::load(const std::string& model_name,
                               const ModelInfo& model_info,
                               const RecipeOptions& options,
                               bool /*do_not_upgrade*/) {
    LOG(INFO, "AMDGPU-Whisper") << "Loading model: " << model_name << std::endl;
    (void)options;

    device_type_ = DEVICE_GPU;

    // Resolve the locally-installed backend (portable CPython + whisper_server.py).
    std::string install_dir = BackendUtils::get_install_directory(SPEC.recipe, "gpu");
    fs::path py = fs::path(install_dir) / "python" / "python.exe";
    fs::path script = fs::path(install_dir) / "whisper_server.py";
    if (!fs::exists(py)) {
        throw std::runtime_error("Whisper AMD GPU python not found at " + py.string() +
                                 " (install the amdgpu-whisper backend first)");
    }
    if (!fs::exists(script)) {
        throw std::runtime_error("whisper_server.py not found at " + script.string());
    }

    std::string model_path = model_info.resolved_path();
    if (model_path.empty() || !fs::exists(model_path)) {
        throw std::runtime_error("Model directory not found for checkpoint: " + model_info.checkpoint());
    }
    LOG(DEBUG, "AMDGPU-Whisper") << "Using model: " << model_path << std::endl;

    port_ = choose_port();

    std::vector<std::string> args = {
        script.string(),
        "-m", model_path,
        "--port", std::to_string(port_)
    };

    std::vector<std::pair<std::string, std::string>> env_vars;
    // Keep the bundled venv isolated from any system/user site-packages.
    env_vars.push_back({"PYTHONNOUSERSITE", "1"});

    LOG(DEBUG, "AMDGPU-Whisper") << "Starting: " << py.string() << " " << script.string()
                                 << " -m " << model_path << " --port " << port_ << std::endl;

    bool inherit_output = (log_level_ == "info") || is_debug();
    ProcessHandle started_handle = utils::ProcessManager::start_process(
        py.string(), args, "", inherit_output, true, env_vars);
    set_process_handle(started_handle);

    if (!utils::ProcessManager::is_running(started_handle)) {
        throw std::runtime_error("Failed to start whisper_server.py process");
    }

    LOG(DEBUG, "AMDGPU-Whisper") << "Process started, PID: " << started_handle.pid << std::endl;

    if (!wait_for_ready("/health")) {
        const ProcessHandle handle = consume_process_handle_for_cleanup();
        if (has_process_handle(handle)) {
            utils::ProcessManager::stop_process(handle);
        }
        throw std::runtime_error("whisper_server.py failed to start or become ready");
    }

    is_loaded_ = true;
    LOG(INFO, "AMDGPU-Whisper") << "Model loaded on port " << get_backend_port() << std::endl;
}

void WhisperAMDGPUServer::unload() {
    stop_backend_watchdog();
    const ProcessHandle handle = consume_process_handle_for_cleanup();
    if (has_process_handle(handle)) {
        utils::ProcessManager::stop_process(handle);
    }
    is_loaded_ = false;
}

json WhisperAMDGPUServer::audio_transcriptions(const json& request) {
    if (!is_loaded_) {
        throw ModelNotLoadedException("AMDGPU-Whisper");
    }
    if (!request.contains("file_data")) {
        throw std::runtime_error("Missing 'file_data' in transcription request");
    }

    const std::string audio_data = request["file_data"].get<std::string>();
    if (audio_data.empty()) {
        throw std::runtime_error("Empty audio data");
    }

    const std::string url = "http://127.0.0.1:" + std::to_string(get_backend_port()) + "/inference";
    auto res = HttpClient::post(url, audio_data, {{"Content-Type", "audio/wav"}}, 300);
    if (res.status_code != 200) {
        throw std::runtime_error("whisper_server returned HTTP " + std::to_string(res.status_code) +
                                 ": " + res.body);
    }

    // whisper_server returns {"text": "..."} which is already the OpenAI shape.
    return json::parse(res.body);
}

json WhisperAMDGPUServer::chat_completion(const json& /*request*/) {
    return json{{"error", {
        {"message", "Whisper models do not support chat completion. Use audio transcription."},
        {"type", "unsupported_operation"},
        {"code", "model_not_applicable"}
    }}};
}

json WhisperAMDGPUServer::completion(const json& /*request*/) {
    return json{{"error", {
        {"message", "Whisper models do not support text completion. Use audio transcription."},
        {"type", "unsupported_operation"},
        {"code", "model_not_applicable"}
    }}};
}

json WhisperAMDGPUServer::responses(const json& /*request*/) {
    return json{{"error", {
        {"message", "Whisper models do not support responses. Use audio transcription."},
        {"type", "unsupported_operation"},
        {"code", "model_not_applicable"}
    }}};
}

} // namespace backends
} // namespace lemon
