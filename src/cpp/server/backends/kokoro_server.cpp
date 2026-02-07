#include "lemon/backends/kokoro_server.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/audio_types.h"
#include "lemon/utils/http_client.h"
#include "lemon/utils/process_manager.h"
#include "lemon/utils/path_utils.h"
#include "lemon/utils/json_utils.h"
#include "lemon/error_types.h"
#include <httplib.h>
#include <iostream>
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

using namespace lemon::utils;

namespace lemon {
namespace backends {

KokoroServer::KokoroServer(const std::string& log_level, ModelManager* model_manager)
    : WrappedServer("kokoro-server", log_level, model_manager) {

}

KokoroServer::~KokoroServer() {
    unload();
}

// WrappedServer interface
void KokoroServer::install(const std::string& backend) {
    std::string repo = "lemonade-sdk/Kokoros";
    std::string filename;
    std::string expected_version = BackendUtils::get_backend_version(SPEC.recipe, backend);

#ifdef _WIN32
    filename = "kokoros-windows-x86_64.tar.gz";
#elif defined(__linux__)
    filename = "kokoros-linux-x86_64.tar.gz";  // Linux binary
#else
    throw std::runtime_error("Unsupported platform for kokoros");
#endif

    BackendUtils::install_from_github(SPEC, expected_version, repo, filename, backend);
}

void KokoroServer::load(const std::string& model_name, const ModelInfo& model_info, const RecipeOptions& options, bool do_not_upgrade) {
    std::cout << "[KokoroServer] Loading model: " << model_name << std::endl;

    // Install kokoros if needed
    install("cpu");

    // Use pre-resolved model path
    fs::path model_path = fs::path(model_info.resolved_path());
    if (model_path.empty() || !fs::exists(model_path)) {
        throw std::runtime_error("Model file not found for checkpoint: " + model_info.checkpoint());
    }

    json model_index;

    try {
        std::cout << "[KokoroServer] Reading " << model_path.filename() << std::endl;
        model_index = JsonUtils::load_from_file(model_path.string());
    } catch (const std::exception& e) {
        throw std::runtime_error("Warning: Could not load " + model_path.filename().string() + ": " + e.what());
    }

    std::cout << "[KokoroServer] Using model: " << model_index["model"] << std::endl;

    // Get koko executable path
    std::string exe_path = BackendUtils::get_backend_binary_path(SPEC, "cpu");

    // Choose a port
    port_ = choose_port();
    if (port_ == 0) {
        throw std::runtime_error("Failed to find an available port");
    }

    std::cout << "[KokoroServer] Starting server on port " << port_ << std::endl;

    std::vector<std::pair<std::string, std::string>> env_vars;
    fs::path exe_dir = fs::path(exe_path).parent_path();
    env_vars.push_back({"ESPEAK_DATA_PATH", exe_dir.string() + "espeak-ng-data"});
#ifndef _WIN32
    std::string lib_path = exe_dir.string();
    // Preserve existing LD_LIBRARY_PATH if it exists
    const char* existing_ld_path = std::getenv("LD_LIBRARY_PATH");
    if (existing_ld_path && strlen(existing_ld_path) > 0) {
        lib_path = lib_path + ":" + std::string(existing_ld_path);
    }

    env_vars.push_back({"LD_LIBRARY_PATH", lib_path});
    std::cout << "[KokoroServer] Setting LD_LIBRARY_PATH=" << lib_path << std::endl;
#endif

    // Build command line arguments
    // Note: Don't include exe_path here - ProcessManager::start_process already handles it
    fs::path model_dir = model_path.parent_path();
    std::vector<std::string> args = {
        "-m", (model_dir / model_index["model"]).string(),
        "-d", (model_dir / model_index["voices"]).string(),
        "openai",
        "--ip", "127.0.0.1",
        "--port", std::to_string(port_)
    };

    // Launch the subprocess
    process_handle_ = utils::ProcessManager::start_process(
        exe_path,
        args,
        "",     // working_dir (empty = current)
        is_debug(),  // inherit_output
        false,
        env_vars
    );

    if (process_handle_.pid == 0) {
        throw std::runtime_error("Failed to start koko process");
    }

    std::cout << "[KokoroServer] Process started with PID: " << process_handle_.pid << std::endl;

    // Wait for server to be ready
    if (!wait_for_ready("/")) {
        unload();
        throw std::runtime_error("koko failed to start or become ready");
    }
}

void KokoroServer::unload() {
    if (process_handle_.pid != 0) {
        std::cout << "[KokoroServer] Stopping server (PID: " << process_handle_.pid << ")" << std::endl;
        utils::ProcessManager::stop_process(process_handle_);
        port_ = 0;
        process_handle_ = {nullptr, 0};
    }
}

// ICompletionServer implementation (not supported - return errors)
json KokoroServer::chat_completion(const json& request) {
    return json{
        {"error", {
            {"message", "Kokoro does not support text completion. Use audio speech endpoints instead."},
            {"type", "unsupported_operation"},
            {"code", "model_not_applicable"}
        }}
    };
}

json KokoroServer::completion(const json& request) {
    return json{
        {"error", {
            {"message", "Kokoro does not support text completion. Use audio speech endpoints instead."},
            {"type", "unsupported_operation"},
            {"code", "model_not_applicable"}
        }}
    };
}

json KokoroServer::responses(const json& request) {
    return json{
        {"error", {
            {"message", "Kokoro does not support text completion. Use audio speech endpoints instead."},
            {"type", "unsupported_operation"},
            {"code", "model_not_applicable"}
        }}
    };
}

void KokoroServer::audio_speech(const json& request, httplib::DataSink& sink) {
    json tts_request = request;
    tts_request["model"] = "kokoro";

    // OpenAI does not define "stream" for the speech endpoint
    // relying solely on stream_format. Kokoros requires this boolean
    if (request.contains("stream_format")) {
        tts_request["stream"] = true;
    }

    forward_streaming_request("/v1/audio/speech", tts_request.dump(), sink, false);
}

} // namespace backends
} // namespace lemon
