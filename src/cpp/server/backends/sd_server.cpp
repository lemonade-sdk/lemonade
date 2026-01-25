#include "lemon/backends/sd_server.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/utils/http_client.h"
#include "lemon/utils/process_manager.h"
#include "lemon/utils/path_utils.h"
#include "lemon/utils/json_utils.h"
#include "lemon/error_types.h"
#include <httplib.h>
#include <iostream>
#include <filesystem>
#include <thread>
#include <chrono>

namespace fs = std::filesystem;
using namespace lemon::utils;

namespace lemon {
namespace backends {

// Helper to get stable-diffusion.cpp version from configuration
static std::string get_sd_version() {
    std::string config_path = utils::get_resource_path("resources/backend_versions.json");

    try {
        json config = utils::JsonUtils::load_from_file(config_path);

        if (!config.contains("sd-cpp") || !config["sd-cpp"].is_string()) {
            throw std::runtime_error("backend_versions.json is missing 'sd-cpp' version");
        }

        std::string version = config["sd-cpp"].get<std::string>();
        std::cout << "[SDServer] Using sd-cpp version from config: " << version << std::endl;
        return version;

    } catch (const std::exception& e) {
        std::cerr << "\n" << std::string(70, '=') << std::endl;
        std::cerr << "ERROR: Failed to load sd-cpp version from configuration" << std::endl;
        std::cerr << std::string(70, '=') << std::endl;
        std::cerr << "\nConfig file: " << config_path << std::endl;
        std::cerr << "Error: " << e.what() << std::endl;
        std::cerr << "\nThe backend_versions.json file is required and must contain a valid" << std::endl;
        std::cerr << "'sd-cpp' version string." << std::endl;
        std::cerr << std::string(70, '=') << std::endl << std::endl;
        throw;
    }
}

// Helper to get the install directory for sd executable
static std::string get_sd_install_dir() {
    return (fs::path(get_downloaded_bin_dir()) / "sd-cpp").string();
}

SDServer::SDServer(const std::string& log_level,
                   ModelManager* model_manager)
    : WrappedServer("sd-server", log_level, model_manager) {

    if (is_debug()) {
        std::cout << "[SDServer] Created with log_level=" << log_level << std::endl;
    }
}

SDServer::~SDServer() {
    unload();
}

bool SDServer::wait_for_ready(int timeout_seconds) {
    std::cout << "[SDServer] Waiting for server to be ready on port " << port_ << "..." << std::endl;

    auto start = std::chrono::steady_clock::now();

    while (true) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start).count();

        if (elapsed >= timeout_seconds) {
            std::cerr << "[SDServer] Timeout waiting for server to be ready after "
                      << timeout_seconds << "s" << std::endl;
            return false;
        }

        // Check if process is still running
        if (!utils::ProcessManager::is_running(process_handle_)) {
            int exit_code = utils::ProcessManager::get_exit_code(process_handle_);
            std::cerr << "[SDServer] Server process exited unexpectedly with code: "
                      << exit_code << std::endl;
            return false;
        }

        try {
            httplib::Client client("127.0.0.1", port_);
            client.set_connection_timeout(2);
            client.set_read_timeout(2);

            auto response = client.Get("/");
            if (response && response->status == 200) {
                std::cout << "[SDServer] Server is ready!" << std::endl;
                return true;
            }
            if (response) {
                std::cout << "[SDServer] Got response with status " << response->status
                          << ", waiting for 200..." << std::endl;
            }
        } catch (const std::exception& e) {
            if (is_debug()) {
                std::cout << "[SDServer] Health check failed: " << e.what() << std::endl;
            }
        } catch (...) {
            // Server not ready yet
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

std::string SDServer::find_executable_in_install_dir(const std::string& install_dir) {
    if (!fs::exists(install_dir)) {
        return "";
    }

#ifdef _WIN32
    fs::path exe_path = fs::path(install_dir) / "sd-server.exe";
#else
    fs::path exe_path = fs::path(install_dir) / "sd-server";
#endif

    if (fs::exists(exe_path)) {
        return exe_path.string();
    }

    return "";
}


void SDServer::install(const std::string& /* backend */) {
    std::string install_dir = get_sd_install_dir();

    // Check if already installed
    std::string exe_path = find_executable_in_install_dir(install_dir);
    if (!exe_path.empty()) {
        std::cout << "[SDServer] sd-server already installed at: " << exe_path << std::endl;
        return;
    }

    std::cout << "[SDServer] Installing stable-diffusion.cpp server..." << std::endl;

    // Get version and construct download URL
    std::string expected_version = get_sd_version();
    std::string repo = "leejet/stable-diffusion.cpp";

    // Transform version for URL (master-NNN-HASH -> master-HASH)
    std::string short_version = expected_version;
    size_t first_dash = expected_version.find('-');
    if (first_dash != std::string::npos) {
        size_t second_dash = expected_version.find('-', first_dash + 1);
        if (second_dash != std::string::npos) {
            short_version = expected_version.substr(0, first_dash) + "-" +
                           expected_version.substr(second_dash + 1);
        }
    }

    std::string filename;
#ifdef _WIN32
    // Windows CPU build with AVX2
    filename = "sd-" + short_version + "-bin-win-avx2-x64.zip";
#elif defined(__linux__)
    // Linux build
    filename = "sd-" + short_version + "-bin-Linux-Ubuntu-24.04-x86_64.zip";
#elif defined(__APPLE__)
    // macOS ARM build
    filename = "sd-" + short_version + "-bin-Darwin-macOS-15.7.2-arm64.zip";
#else
    throw std::runtime_error("Unsupported platform for stable-diffusion.cpp");
#endif

    std::string url = "https://github.com/" + repo + "/releases/download/" +
                     expected_version + "/" + filename;

    // Download ZIP to cache directory
    fs::path cache_dir = model_manager_ ? fs::path(model_manager_->get_hf_cache_dir()) : fs::temp_directory_path();
    fs::create_directories(cache_dir);
    std::string zip_path = (cache_dir / ("sd_" + expected_version + ".zip")).string();

    std::cout << "[SDServer] Downloading from: " << url << std::endl;

    // Download with progress
    auto download_result = utils::HttpClient::download_file(
        url,
        zip_path,
        utils::create_throttled_progress_callback()
    );

    if (!download_result.success) {
        throw std::runtime_error("Failed to download stable-diffusion.cpp from: " + url +
                                " - " + download_result.error_message);
    }
    std::cout << std::endl;

    std::cout << "[SDServer] Download complete!" << std::endl;

    // Verify file size
    auto file_size = fs::file_size(zip_path);
    std::cout << "[SDServer] Downloaded ZIP file size: " << (file_size / 1024 / 1024) << " MB" << std::endl;

    // Extract the ZIP
    if (!backends::BackendUtils::extract_archive(zip_path, install_dir, "SDServer")) {
        throw std::runtime_error("Failed to extract ZIP file");
    }

    // Verify extraction
    exe_path = find_executable_in_install_dir(install_dir);
    if (exe_path.empty()) {
        throw std::runtime_error("sd-server executable not found after extraction");
    }

#ifndef _WIN32
    // Make executable on Unix
    chmod(exe_path.c_str(), 0755);
#endif

    std::cout << "[SDServer] Executable installed at: " << exe_path << std::endl;
    std::cout << "[SDServer] Installation complete!" << std::endl;

    // Clean up ZIP file
    try {
        fs::remove(zip_path);
    } catch (...) {
        // Ignore cleanup errors
    }
}

std::string SDServer::download_model(const std::string& checkpoint,
                                     const std::string& /* mmproj */,
                                     bool do_not_upgrade) {
    if (!model_manager_) {
        throw std::runtime_error("ModelManager not available for model download");
    }

    std::cout << "[SDServer] Downloading model: " << checkpoint << std::endl;

    // Use ModelManager's download_model which handles HuggingFace downloads
    model_manager_->download_model(
        checkpoint,  // model_name
        checkpoint,  // checkpoint
        "sd-cpp",    // recipe
        false,       // reasoning
        false,       // vision
        false,       // embedding
        false,       // reranking
        true,        // image
        "",          // mmproj
        do_not_upgrade
    );

    // Get the resolved path from model info
    ModelInfo info = model_manager_->get_model_info(checkpoint);
    std::string model_path = info.resolved_path;

    if (model_path.empty()) {
        throw std::runtime_error("Failed to download SD model: " + checkpoint);
    }

    std::cout << "[SDServer] Model downloaded to: " << model_path << std::endl;
    return model_path;
}

void SDServer::load(const std::string& model_name,
                    const ModelInfo& model_info,
                    const RecipeOptions& /* options */,
                    bool /* do_not_upgrade */) {
    std::cout << "[SDServer] Loading model: " << model_name << std::endl;

    // Install sd-server if needed
    install("");

    // Get model path
    std::string model_path = model_info.resolved_path;
    if (model_path.empty()) {
        throw std::runtime_error("Model file not found for checkpoint: " + model_info.checkpoint);
    }

    // For SD models, checkpoint format is "repo:filename" - find the actual file
    std::string target_filename;
    size_t colon_pos = model_info.checkpoint.find(':');
    if (colon_pos != std::string::npos) {
        target_filename = model_info.checkpoint.substr(colon_pos + 1);
    }

    // Navigate HuggingFace cache structure if needed
    if (fs::is_directory(model_path)) {
        if (!target_filename.empty()) {
            std::cout << "[SDServer] Searching for " << target_filename << " in " << model_path << std::endl;
        }

        fs::path snapshots_dir = fs::path(model_path) / "snapshots";
        if (fs::exists(snapshots_dir) && fs::is_directory(snapshots_dir)) {
            for (const auto& snapshot_entry : fs::directory_iterator(snapshots_dir)) {
                if (snapshot_entry.is_directory()) {
                    if (!target_filename.empty()) {
                        fs::path candidate = snapshot_entry.path() / target_filename;
                        if (fs::exists(candidate) && fs::is_regular_file(candidate)) {
                            model_path = candidate.string();
                            break;
                        }
                    } else {
                        // Search for any .safetensors file
                        for (const auto& file_entry : fs::directory_iterator(snapshot_entry.path())) {
                            if (file_entry.is_regular_file()) {
                                std::string fname = file_entry.path().filename().string();
                                if (fname.size() > 12 && fname.substr(fname.size() - 12) == ".safetensors") {
                                    model_path = file_entry.path().string();
                                    break;
                                }
                            }
                        }
                        if (!fs::is_directory(model_path)) break;
                    }
                }
            }
        }
    }

    if (fs::is_directory(model_path)) {
        throw std::runtime_error("Model path is a directory, not a file: " + model_path);
    }

    if (!fs::exists(model_path)) {
        throw std::runtime_error("Model file does not exist: " + model_path);
    }

    std::cout << "[SDServer] Using model: " << model_path << std::endl;
    model_path_ = model_path;

    // Get sd-server executable path
    std::string exe_path = find_executable_in_install_dir(get_sd_install_dir());
    if (exe_path.empty()) {
        throw std::runtime_error("sd-server executable not found");
    }

    // Choose a port
    port_ = choose_port();
    if (port_ == 0) {
        throw std::runtime_error("Failed to find an available port");
    }

    std::cout << "[SDServer] Starting server on port " << port_ << std::endl;

    // Build command line arguments
    std::vector<std::string> args = {
        "-m", model_path_,
        "--listen-port", std::to_string(port_)
    };

    if (is_debug()) {
        args.push_back("-v");
    }

    // Set up environment variables for Linux (LD_LIBRARY_PATH)
    std::vector<std::pair<std::string, std::string>> env_vars;
#ifndef _WIN32
    fs::path exe_dir = fs::path(exe_path).parent_path();
    std::string lib_path = exe_dir.string();

    const char* existing_ld_path = std::getenv("LD_LIBRARY_PATH");
    if (existing_ld_path && strlen(existing_ld_path) > 0) {
        lib_path = lib_path + ":" + std::string(existing_ld_path);
    }

    env_vars.push_back({"LD_LIBRARY_PATH", lib_path});
    if (is_debug()) {
        std::cout << "[SDServer] Setting LD_LIBRARY_PATH=" << lib_path << std::endl;
    }
#endif

    // Launch the server process
    process_handle_ = utils::ProcessManager::start_process(
        exe_path,
        args,
        "",     // working_dir (empty = current)
        is_debug(),  // inherit_output
        false,  // filter_health_logs
        env_vars
    );

    if (process_handle_.pid == 0) {
        throw std::runtime_error("Failed to start sd-server process");
    }

    std::cout << "[SDServer] Process started with PID: " << process_handle_.pid << std::endl;

    // Wait for server to be ready
    if (!wait_for_ready()) {
        unload();
        throw std::runtime_error("sd-server failed to start or become ready");
    }

    std::cout << "[SDServer] Server is ready at http://127.0.0.1:" << port_ << std::endl;
}

void SDServer::unload() {
    if (process_handle_.pid != 0) {
        std::cout << "[SDServer] Stopping server (PID: " << process_handle_.pid << ")" << std::endl;
        utils::ProcessManager::stop_process(process_handle_);
        process_handle_ = {nullptr, 0};
        port_ = 0;
        model_path_.clear();
    }
}

// ICompletionServer implementation - not supported for image generation
json SDServer::chat_completion(const json& /* request */) {
    return ErrorResponse::from_exception(
        UnsupportedOperationException("Chat completion", "sd-cpp (image generation model)")
    );
}

json SDServer::completion(const json& /* request */) {
    return ErrorResponse::from_exception(
        UnsupportedOperationException("Text completion", "sd-cpp (image generation model)")
    );
}

json SDServer::responses(const json& /* request */) {
    return ErrorResponse::from_exception(
        UnsupportedOperationException("Responses", "sd-cpp (image generation model)")
    );
}

json SDServer::image_generations(const json& request) {
    // Build request - sd-server uses OpenAI-compatible format
    json sd_request = request;

    // sd-server requires extra params (steps, sample_method, scheduler) to be
    // embedded in the prompt as <sd_cpp_extra_args>JSON</sd_cpp_extra_args>
    // See PR #1173: https://github.com/leejet/stable-diffusion.cpp/pull/1173
    json extra_args;
    if (request.contains("steps")) {
        extra_args["steps"] = request["steps"];
    }
    if (request.contains("cfg_scale")) {
        extra_args["cfg_scale"] = request["cfg_scale"];
    }
    if (request.contains("seed")) {
        extra_args["seed"] = request["seed"];
    }
    if (request.contains("sample_method")) {
        extra_args["sample_method"] = request["sample_method"];
    }
    if (request.contains("scheduler")) {
        extra_args["scheduler"] = request["scheduler"];
    }

    // Append extra args to prompt if any were specified
    if (!extra_args.empty()) {
        std::string prompt = sd_request.value("prompt", "");
        prompt += " <sd_cpp_extra_args>" + extra_args.dump() + "</sd_cpp_extra_args>";
        sd_request["prompt"] = prompt;
    }

    if (is_debug()) {
        std::cout << "[SDServer] Forwarding request to sd-server: "
                  << sd_request.dump(2) << std::endl;
    }

    // Use base class forward_request with 10 minute timeout for image generation
    return forward_request("/v1/images/generations", sd_request, 600);
}

} // namespace backends
} // namespace lemon
