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

static std::string get_kokoro_install_dir(const std::string& backend) {
    return (fs::path(get_downloaded_bin_dir()) / "kokoro" / backend).string();
}

// WrappedServer interface
void KokoroServer::install(const std::string& backend) {

}

std::string KokoroServer::download_model(const std::string& checkpoint, const std::string& mmproj, bool do_not_upgrade) {
    // Download .bin file from Hugging Face using ModelManager
    if (!model_manager_) {
        throw std::runtime_error("ModelManager not available for model download");
    }

    std::cout << "[KokoroServer] Downloading model from: " << checkpoint << std::endl;

    // Use ModelManager's download_model which handles HuggingFace downloads
    // The download is triggered through the model registry system
    // Model path will be resolved via ModelInfo.resolved_path
    model_manager_->download_model(
        checkpoint,  // model_name
        checkpoint,  // checkpoint
        "kokoro",    // recipe
        false,       // reasoning
        false,       // vision
        false,       // embedding
        false,       // reranking
        false,       // image
        "",          // mmproj
        do_not_upgrade
    );

    // Get the resolved path from model info
    ModelInfo info = model_manager_->get_model_info(checkpoint);
    std::string model_path = info.resolved_path;

    if (model_path.empty() || !fs::exists(model_path)) {
        throw std::runtime_error("Failed to download Kokoro model: " + checkpoint);
    }

    std::cout << "[KokoroServer] Model downloaded to: " << model_path << std::endl;
    return model_path;
}

bool KokoroServer::wait_for_ready(int timeout_seconds) {
    std::cout << "[KokoroServer] Waiting for server to be ready on port " << port_ << "..." << std::endl;

    auto start = std::chrono::steady_clock::now();

    while (true) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start).count();

        if (elapsed >= timeout_seconds) {
            std::cerr << "[KokoroServer] Timeout waiting for server to be ready after "
                      << timeout_seconds << "s" << std::endl;
            return false;
        }

        // Check if process is still running
        if (!utils::ProcessManager::is_running(process_handle_)) {
            int exit_code = utils::ProcessManager::get_exit_code(process_handle_);
            std::cerr << "[KokoroServer] Server process exited unexpectedly with code: "
                      << exit_code << std::endl;
            return false;
        }

        try {
            httplib::Client client("127.0.0.1", port_);
            client.set_connection_timeout(2);
            client.set_read_timeout(2);

            auto response = client.Get("/");
            if (response && response->status == 200) {
                std::cout << "[KokoroServer] Server is ready!" << std::endl;
                return true;
            }
            if (response) {
                std::cout << "[KokoroServer] Got response with status " << response->status
                          << ", waiting for 200..." << std::endl;
            }
        } catch (const std::exception& e) {
            if (is_debug()) {
                std::cout << "[KokoroServer] Health check failed: " << e.what() << std::endl;
            }
        } catch (...) {
            // Server not ready yet
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

void KokoroServer::load(const std::string& model_name, const ModelInfo& model_info, const RecipeOptions& options, bool do_not_upgrade) {
    std::cout << "[KokoroServer] Loading model: " << model_name << std::endl;

    // Install whisper-server if needed
    install("cpu");

    // Use pre-resolved model path
    fs::path model_path = fs::path(model_info.resolved_path);
    if (model_path.empty() || !fs::exists(model_path)) {
        throw std::runtime_error("Model file not found for checkpoint: " + model_info.checkpoint);
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
    std::string exe_path = get_kokoro_server_path();
    if (exe_path.empty()) {
        throw std::runtime_error("koko executable not found");
    }

    // Choose a port
    port_ = choose_port();
    if (port_ == 0) {
        throw std::runtime_error("Failed to find an available port");
    }

    std::cout << "[KokoroServer] Starting server on port " << port_ << std::endl;

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
        is_debug()  // inherit_output
    );

    if (process_handle_.pid == 0) {
        throw std::runtime_error("Failed to start koko process");
    }

    std::cout << "[KokoroServer] Process started with PID: " << process_handle_.pid << std::endl;

    // Wait for server to be ready
    if (!wait_for_ready()) {
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
    forward_streaming_request("/v1/audio/speech", tts_request.dump(), sink, false);
}

std::string KokoroServer::get_kokoro_server_path() {
    std::string exe_path = find_external_kokoro_server();

    if (!exe_path.empty()) {
        return exe_path;
    }

    std::string install_dir = get_kokoro_install_dir("cpu");
    return find_executable_in_install_dir(install_dir);
}

std::string KokoroServer::find_executable_in_install_dir(const std::string& install_dir) {
    // Look for whisper-server executable
#ifdef _WIN32
    std::vector<std::string> exe_names = {"koko.exe"};
    std::vector<std::string> subdirs = {"release", "bin", ""};
#else
    std::vector<std::string> exe_names = {"koko", ""};
    std::vector<std::string> subdirs = {"release", "bin", ""};
#endif

    for (const auto& subdir : subdirs) {
        for (const auto& exe_name : exe_names) {
            fs::path exe_path;
            if (subdir.empty()) {
                exe_path = fs::path(install_dir) / exe_name;
            } else {
                exe_path = fs::path(install_dir) / subdir / exe_name;
            }
            if (fs::exists(exe_path)) {
                return exe_path.string();
            }
        }
    }

    return "";
}

std::string KokoroServer::find_external_kokoro_server() {
    const char* kokoro_bin_env = std::getenv("LEMONADE_KOKORO_CPU_BIN");
    if (!kokoro_bin_env) {
        return "";
    }

    std::string kokoro_bin = std::string(kokoro_bin_env);

    return fs::exists(kokoro_bin) ? kokoro_bin : "";
}

} // namespace backends
} // namespace lemon
