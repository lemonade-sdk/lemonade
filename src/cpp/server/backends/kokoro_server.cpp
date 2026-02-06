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

static std::string get_kokoro_version(const std::string& backend) {
    std::string config_path = utils::get_resource_path("resources/backend_versions.json");

    json config = utils::JsonUtils::load_from_file(config_path);

    if (!config.contains("kokoro") || !config["kokoro"].is_object()) {
        throw std::runtime_error("backend_versions.json is missing 'kokoro' section");
    }

    const auto& kokoro_config = config["kokoro"];

    if (!kokoro_config.contains(backend) || !kokoro_config[backend].is_string()) {
        throw std::runtime_error("backend_versions.json is missing version for backend: " + backend);
    }

    std::string version = kokoro_config[backend].get<std::string>();
    std::cout << "[KokoroServer] Using " << backend << " version from config: " << version << std::endl;
    return version;
}

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
    std::string install_dir;
    std::string version_file;
    std::string expected_version;
    std::string exe_path = find_external_kokoro_server();
    bool needs_install = exe_path.empty();

    if (needs_install) {
        install_dir = get_kokoro_install_dir(backend);
        version_file = (fs::path(install_dir) / "version.txt").string();

        // Get expected version from config
        expected_version = get_kokoro_version(backend);

        // Check if already installed with correct version
        exe_path = find_executable_in_install_dir(install_dir);
        needs_install = exe_path.empty();

        if (!needs_install && fs::exists(version_file)) {
            std::string installed_version;

            std::ifstream vf(version_file);
            std::getline(vf, installed_version);
            vf.close();

            if (installed_version != expected_version) {
                std::cout << "[KokoroServer] Upgrading from " << installed_version
                        << " to " << expected_version << std::endl;
                needs_install = true;
                fs::remove_all(install_dir);
            }
        }
    }

    if (needs_install) {
        std::cout << "[KokoroServer] Installing kokoros (version: "
                 << expected_version << ")" << std::endl;

        // Create install directory
        fs::create_directories(install_dir);

        // Determine download URL
        std::string repo = "lemonade-sdk/Kokoros";
        std::string filename;

#ifdef _WIN32
        filename = "kokoros-windows-x86_64.tar.gz";
#elif defined(__linux__)
        filename = "kokoros-linux-x86_64.tar.gz";  // Linux binary
#else
        throw std::runtime_error("Unsupported platform for kokoros");
#endif

        std::string url = "https://github.com/" + repo + "/releases/download/" +
                         expected_version + "/" + filename;

        // Download ZIP to cache directory
        fs::path cache_dir = model_manager_ ? fs::path(model_manager_->get_hf_cache_dir()) : fs::temp_directory_path();
        fs::create_directories(cache_dir);
        std::string zip_path = (cache_dir / ("kokoros_" + expected_version + ".tar.gz")).string();

        std::cout << "[KokoroServer] Downloading from: " << url << std::endl;
        std::cout << "[KokoroServer] Downloading to: " << zip_path << std::endl;

        // Download the file
        auto download_result = utils::HttpClient::download_file(
            url,
            zip_path,
            utils::create_throttled_progress_callback()
        );

        if (!download_result.success) {
            throw std::runtime_error("Failed to download kokoros from: " + url +
                                    " - " + download_result.error_message);
        }

        std::cout << std::endl << "[KokoroServer] Download complete!" << std::endl;

        // Verify the downloaded file
        if (!fs::exists(zip_path)) {
            throw std::runtime_error("Downloaded tarball does not exist: " + zip_path);
        }

        std::uintmax_t file_size = fs::file_size(zip_path);
        std::cout << "[KokoroServer] Downloaded tarball file size: "
                  << (file_size / 1024 / 1024) << " MB" << std::endl;

        // Extract
        if (!backends::BackendUtils::extract_archive(zip_path, install_dir, "KokoroServer")) {
            fs::remove(zip_path);
            fs::remove_all(install_dir);
            throw std::runtime_error("Failed to extract kokoros archive");
        }

        // Verify extraction
        exe_path = find_executable_in_install_dir(install_dir);
        if (exe_path.empty()) {
            std::cerr << "[KokoroServer] ERROR: Extraction completed but executable not found" << std::endl;
            fs::remove(zip_path);
            fs::remove_all(install_dir);
            throw std::runtime_error("Extraction failed: executable not found");
        }

        std::cout << "[KokoroServer] Executable verified at: " << exe_path << std::endl;

        // Save version info
        std::ofstream vf(version_file);
        vf << expected_version;
        vf.close();

#ifndef _WIN32
        // Make executable on Linux/macOS
        chmod(exe_path.c_str(), 0755);
#endif

        // Delete ZIP file
        fs::remove(zip_path);

        std::cout << "[KokoroServer] Installation complete!" << std::endl;
    } else {
        std::cout << "[KokoroServer] Found koko at: " << exe_path << std::endl;
    }
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

    // Install kokoros if needed
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

    // OpenAI does not define "stream" for the speech endpoint
    // relying solely on stream_format. Kokoros requires this boolean
    if (request.contains("stream_format")) {
        tts_request["stream"] = true;
    }

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
    // Look for kokoros executable
#ifdef _WIN32
    std::vector<std::string> exe_names = {"koko.exe"};
    std::vector<std::string> subdirs = {"kokoros-windows-x86_64", "windows-x86_64", ""};
#else
    std::vector<std::string> exe_names = {"koko"};
    std::vector<std::string> subdirs = {"kokoros-linux-x86_64", "linux-x86_64", ""};
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
