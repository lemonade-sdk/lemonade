#include "lemon/backends/sd_server.h"
#include "lemon/utils/http_client.h"
#include "lemon/utils/process_manager.h"
#include "lemon/utils/path_utils.h"
#include "lemon/utils/json_utils.h"
#include "lemon/error_types.h"
#include <iostream>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <vector>
#include <chrono>
#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

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
            // Default version if not in config
            return "master-2c39fd0";
        }

        return config["sd-cpp"].get<std::string>();

    } catch (const std::exception& e) {
        std::cerr << "[SDServer] Warning: Could not load version from config: "
                  << e.what() << std::endl;
        std::cerr << "[SDServer] Using default version: master-2c39fd0" << std::endl;
        return "master-2c39fd0";
    }
}

// Helper to get the base directory for sd binaries
static std::string get_sd_base_dir() {
#ifdef _WIN32
    char exe_path[MAX_PATH];
    GetModuleFileNameA(NULL, exe_path, MAX_PATH);
    fs::path exe_dir = fs::path(exe_path).parent_path();
    return exe_dir.string();
#else
    char exe_path[1024];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len != -1) {
        exe_path[len] = '\0';
        fs::path exe_dir = fs::path(exe_path).parent_path();

        // If we're in /usr/local/bin, use /usr/local/share/lemonade-server
        if (exe_dir == "/usr/local/bin" || exe_dir == "/usr/bin") {
            if (fs::exists("/usr/local/share/lemonade-server")) {
                return "/usr/local/share/lemonade-server";
            }
            if (fs::exists("/usr/share/lemonade-server")) {
                return "/usr/share/lemonade-server";
            }
        }

        return exe_dir.string();
    }
    return ".";
#endif
}

// Helper to get the install directory for sd executable
static std::string get_sd_install_dir() {
    return (fs::path(get_sd_base_dir()) / "sd-cpp").string();
}

// Helper to run a process safely without shell injection vulnerabilities
static int run_process_safe(const std::vector<std::string>& args) {
#ifdef _WIN32
    if (args.empty()) return -1;

    // Build command line with proper quoting for Windows
    std::string cmdline;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) cmdline += " ";
        // Quote arguments that contain spaces or special characters
        bool needs_quotes = args[i].find_first_of(" \t\"") != std::string::npos;
        if (needs_quotes) {
            cmdline += "\"";
            // Escape embedded quotes
            for (char c : args[i]) {
                if (c == '"') cmdline += "\\\"";
                else cmdline += c;
            }
            cmdline += "\"";
        } else {
            cmdline += args[i];
        }
    }

    STARTUPINFOA si = {};
    PROCESS_INFORMATION pi = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    // CreateProcess needs a mutable string
    std::vector<char> cmdline_buf(cmdline.begin(), cmdline.end());
    cmdline_buf.push_back('\0');

    if (!CreateProcessA(NULL, cmdline_buf.data(), NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        return -1;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<int>(exit_code);
#else
    pid_t pid = fork();
    if (pid == -1) {
        return -1;
    } else if (pid == 0) {
        // Child process
        std::vector<char*> argv;
        for (const auto& arg : args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127); // exec failed
    } else {
        // Parent process
        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status)) {
            return WEXITSTATUS(status);
        }
        return -1;
    }
#endif
}

// Helper to extract ZIP files
static bool extract_zip(const std::string& zip_path, const std::string& dest_dir) {
    std::cout << "[SDServer] Extracting ZIP to " << dest_dir << std::endl;

#ifdef _WIN32
    // Try PowerShell Expand-Archive (safest, uses argument array)
    int result = run_process_safe({
        "powershell.exe", "-NoProfile", "-Command",
        "Expand-Archive -Path '" + zip_path + "' -DestinationPath '" + dest_dir + "' -Force"
    });
    if (result == 0) {
        return true;
    }
    std::cerr << "[SDServer] PowerShell extraction failed with code: " << result << std::endl;

    // Try Windows tar (supports zip on Windows 10 1903+)
    result = run_process_safe({"tar", "-xf", zip_path, "-C", dest_dir});
    if (result == 0) {
        return true;
    }
    std::cerr << "[SDServer] All extraction methods failed" << std::endl;
    return false;
#else
    int result = run_process_safe({"unzip", "-o", zip_path, "-d", dest_dir});
    return result == 0;
#endif
}

SDServer::SDServer(const std::string& log_level, ModelManager* model_manager,
                   bool save_images, const std::string& images_dir)
    : WrappedServer("sd-server", log_level, model_manager),
      save_images_(save_images), images_dir_(images_dir) {

    // Create directory for output images
    if (save_images_) {
        // Use specified directory or default to ./generated_images
        if (images_dir_.empty()) {
            temp_dir_ = fs::current_path() / "generated_images";
        } else {
            temp_dir_ = fs::path(images_dir_);
        }
        std::cout << "[SDServer] Images will be saved to: " << temp_dir_.string() << std::endl;
    } else {
        // Use temp directory for transient images
        temp_dir_ = fs::temp_directory_path() / "lemonade_images";
    }
    fs::create_directories(temp_dir_);
}

SDServer::~SDServer() {
    unload();

    // Only clean up temp directory if we're not saving images
    if (!save_images_) {
        try {
            if (fs::exists(temp_dir_)) {
                fs::remove_all(temp_dir_);
            }
        } catch (const std::exception& e) {
            std::cerr << "[SDServer] Warning: Could not clean up temp directory: "
                      << e.what() << std::endl;
        }
    }
}

std::string SDServer::find_executable_in_install_dir(const std::string& install_dir) {
    // Look for sd executable
    // The stable-diffusion.cpp releases use 'sd-cli' as the CLI executable name
#ifdef _WIN32
    std::vector<std::string> exe_names = {"sd-cli.exe", "sd.exe", "stable-diffusion.exe"};
    std::vector<std::string> subdirs = {"bin", ""};
#else
    std::vector<std::string> exe_names = {"sd-cli", "sd", "stable-diffusion"};
    std::vector<std::string> subdirs = {"bin", ""};
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

std::string SDServer::find_external_sd_executable() {
    const char* sd_bin_env = std::getenv("LEMONADE_SDCPP_BIN");
    if (!sd_bin_env) {
        return "";
    }

    std::string sd_bin = std::string(sd_bin_env);

    return fs::exists(sd_bin) ? sd_bin : "";
}

std::string SDServer::get_sd_executable_path() {
    std::string exe_path = find_external_sd_executable();

    if (!exe_path.empty()) {
        return exe_path;
    }

    std::string install_dir = get_sd_install_dir();
    return find_executable_in_install_dir(install_dir);
}

void SDServer::install(const std::string& backend) {
    std::string install_dir;
    std::string version_file;
    std::string expected_version;
    std::string exe_path = find_external_sd_executable();
    bool needs_install = exe_path.empty();

    if (needs_install) {
        install_dir = get_sd_install_dir();
        version_file = (fs::path(install_dir) / "version.txt").string();

        // Get expected version from config
        expected_version = get_sd_version();

        // Check if already installed with correct version
        exe_path = find_executable_in_install_dir(install_dir);
        needs_install = exe_path.empty();

        if (!needs_install && fs::exists(version_file)) {
            std::string installed_version;

            std::ifstream vf(version_file);
            std::getline(vf, installed_version);
            vf.close();

            if (installed_version != expected_version) {
                std::cout << "[SDServer] Upgrading from " << installed_version
                        << " to " << expected_version << std::endl;
                needs_install = true;
                fs::remove_all(install_dir);
            }
        }
    }

    if (needs_install) {
        std::cout << "[SDServer] Installing stable-diffusion.cpp (version: "
                 << expected_version << ")" << std::endl;

        // Create install directory
        fs::create_directories(install_dir);

        // Determine download URL
        // stable-diffusion.cpp releases are at: https://github.com/leejet/stable-diffusion.cpp/releases
        // Version format: "master-453-4ff2c8c" -> asset uses "master-4ff2c8c" (skip build number)
        std::string repo = "leejet/stable-diffusion.cpp";
        std::string filename;

        // Extract short version for asset name (e.g., "master-453-4ff2c8c" -> "master-4ff2c8c")
        std::string short_version = expected_version;
        size_t first_dash = expected_version.find('-');
        if (first_dash != std::string::npos) {
            size_t second_dash = expected_version.find('-', first_dash + 1);
            if (second_dash != std::string::npos) {
                // Format: master-XXX-HASH -> master-HASH
                short_version = expected_version.substr(0, first_dash) + "-" +
                               expected_version.substr(second_dash + 1);
            }
        }

#ifdef _WIN32
        // Windows Vulkan build: sd-master-4ff2c8c-bin-win-vulkan-x64.zip
        filename = "sd-" + short_version + "-bin-win-vulkan-x64.zip";
#elif defined(__linux__)
        // Linux build: sd-master-4ff2c8c-bin-Linux-Ubuntu-24.04-x86_64.zip
        filename = "sd-" + short_version + "-bin-Linux-Ubuntu-24.04-x86_64.zip";
#elif defined(__APPLE__)
        // macOS ARM build: sd-master-4ff2c8c-bin-Darwin-macOS-15.7.2-arm64.zip
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
        std::cout << "[SDServer] Downloading to: " << zip_path << std::endl;

        // Download the file
        auto download_result = utils::HttpClient::download_file(
            url,
            zip_path,
            utils::create_throttled_progress_callback()
        );

        if (!download_result.success) {
            throw std::runtime_error("Failed to download stable-diffusion.cpp from: " + url +
                                    " - " + download_result.error_message);
        }

        std::cout << std::endl << "[SDServer] Download complete!" << std::endl;

        // Verify the downloaded file
        if (!fs::exists(zip_path)) {
            throw std::runtime_error("Downloaded ZIP file does not exist: " + zip_path);
        }

        std::uintmax_t file_size = fs::file_size(zip_path);
        std::cout << "[SDServer] Downloaded ZIP file size: "
                  << (file_size / 1024 / 1024) << " MB" << std::endl;

        const std::uintmax_t MIN_ZIP_SIZE = 1024 * 1024;  // 1 MB
        if (file_size < MIN_ZIP_SIZE) {
            std::cerr << "[SDServer] ERROR: Downloaded file is too small" << std::endl;
            fs::remove(zip_path);
            throw std::runtime_error("Downloaded file is too small, likely corrupted");
        }

        // Extract
        if (!extract_zip(zip_path, install_dir)) {
            fs::remove(zip_path);
            fs::remove_all(install_dir);
            throw std::runtime_error("Failed to extract stable-diffusion.cpp archive");
        }

        // Verify extraction
        exe_path = find_executable_in_install_dir(install_dir);
        if (exe_path.empty()) {
            std::cerr << "[SDServer] ERROR: Extraction completed but executable not found" << std::endl;
            fs::remove(zip_path);
            fs::remove_all(install_dir);
            throw std::runtime_error("Extraction failed: executable not found");
        }

        std::cout << "[SDServer] Executable verified at: " << exe_path << std::endl;

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

        std::cout << "[SDServer] Installation complete!" << std::endl;
    } else {
        std::cout << "[SDServer] Found stable-diffusion.cpp at: " << exe_path << std::endl;
    }
}

std::string SDServer::download_model(const std::string& checkpoint,
                                     const std::string& mmproj,
                                     bool do_not_upgrade) {
    // Parse checkpoint: "stabilityai/stable-diffusion-v1-5:v1-5-pruned.safetensors"
    // or just "model.safetensors" for local files
    std::string repo, filename;
    size_t colon_pos = checkpoint.find(':');

    if (colon_pos != std::string::npos) {
        repo = checkpoint.substr(0, colon_pos);
        filename = checkpoint.substr(colon_pos + 1);
    } else {
        throw std::runtime_error("Invalid checkpoint format. Expected 'repo:filename'");
    }

    // Download model file from Hugging Face using ModelManager
    if (!model_manager_) {
        throw std::runtime_error("ModelManager not available for model download");
    }

    std::cout << "[SDServer] Downloading model: " << filename << " from " << repo << std::endl;

    // Use ModelManager's download_model which handles HuggingFace downloads
    model_manager_->download_model(
        checkpoint,  // model_name
        checkpoint,  // checkpoint
        "sd-cpp",    // recipe
        false,       // reasoning
        false,       // vision
        false,       // embedding
        false,       // reranking
        true,        // image (SD models are image generation models)
        "",          // mmproj
        do_not_upgrade
    );

    // Get the resolved path from model info
    ModelInfo info = model_manager_->get_model_info(checkpoint);
    std::string model_path = info.resolved_path;

    if (model_path.empty() || !fs::exists(model_path)) {
        throw std::runtime_error("Failed to download SD model: " + checkpoint);
    }

    std::cout << "[SDServer] Model downloaded to: " << model_path << std::endl;
    return model_path;
}

void SDServer::load(const std::string& model_name,
                    const ModelInfo& model_info,
                    int ctx_size,
                    bool do_not_upgrade,
                    const std::string& /* llamacpp_backend */,
                    const std::string& /* llamacpp_args */) {
    // Note: llamacpp_backend and llamacpp_args are not used for sd
    // They're included for API compatibility with WrappedServer interface

    std::cout << "[SDServer] Loading model: " << model_name << std::endl;

    // Install sd executable if needed
    install("");

    // Use pre-resolved model path
    std::string model_path = model_info.resolved_path;
    if (model_path.empty()) {
        throw std::runtime_error("Model file not found for checkpoint: " + model_info.checkpoint);
    }

    // For SD models, the checkpoint format is "repo:filename" (e.g., "stabilityai/sd-turbo:sd_turbo.safetensors")
    // The resolved_path may be the HuggingFace cache directory, we need to find the actual file
    std::string target_filename;
    size_t colon_pos = model_info.checkpoint.find(':');
    if (colon_pos != std::string::npos) {
        target_filename = model_info.checkpoint.substr(colon_pos + 1);
    }

    // Check if resolved_path is a directory (HuggingFace cache structure)
    if (fs::is_directory(model_path)) {
        if (!target_filename.empty()) {
            std::cout << "[SDServer] Searching for " << target_filename << " in " << model_path << std::endl;
        } else {
            std::cout << "[SDServer] Searching for .safetensors file in " << model_path << std::endl;
        }

        // Search in HuggingFace cache structure: models--org--repo/snapshots/*/filename
        fs::path snapshots_dir = fs::path(model_path) / "snapshots";
        if (fs::exists(snapshots_dir) && fs::is_directory(snapshots_dir)) {
            for (const auto& snapshot_entry : fs::directory_iterator(snapshots_dir)) {
                if (snapshot_entry.is_directory()) {
                    if (!target_filename.empty()) {
                        // Look for specific file
                        fs::path candidate = snapshot_entry.path() / target_filename;
                        if (fs::exists(candidate) && fs::is_regular_file(candidate)) {
                            model_path = candidate.string();
                            std::cout << "[SDServer] Found model file: " << model_path << std::endl;
                            break;
                        }
                    } else {
                        // Search for any .safetensors file
                        for (const auto& file_entry : fs::directory_iterator(snapshot_entry.path())) {
                            if (file_entry.is_regular_file()) {
                                std::string fname = file_entry.path().filename().string();
                                if (fname.size() > 12 && fname.substr(fname.size() - 12) == ".safetensors") {
                                    model_path = file_entry.path().string();
                                    std::cout << "[SDServer] Found model file: " << model_path << std::endl;
                                    break;
                                }
                            }
                        }
                        if (!fs::is_directory(model_path)) break;
                    }
                }
            }
        }

        // If still a directory and have target filename, try direct search
        if (fs::is_directory(model_path) && !target_filename.empty()) {
            fs::path direct_file = fs::path(model_path) / target_filename;
            if (fs::exists(direct_file) && fs::is_regular_file(direct_file)) {
                model_path = direct_file.string();
            }
        }
    }

    // Final check - make sure we have an actual file
    if (fs::is_directory(model_path)) {
        throw std::runtime_error("Model path is a directory, not a file. Expected a .safetensors file: " + model_path);
    }

    if (!fs::exists(model_path)) {
        throw std::runtime_error("Model file does not exist: " + model_path);
    }

    std::cout << "[SDServer] Using model: " << model_path << std::endl;
    model_path_ = model_path;

    // Get sd executable path
    std::string exe_path = get_sd_executable_path();
    if (exe_path.empty()) {
        throw std::runtime_error("stable-diffusion.cpp executable not found");
    }

    std::cout << "[SDServer] stable-diffusion.cpp ready at: " << exe_path << std::endl;

    // Note: Unlike whisper-server, sd.cpp is a CLI tool that runs per-request
    // We don't start a long-running server process here
    // Each image_generations call will invoke the CLI
}

void SDServer::unload() {
    model_path_.clear();
    std::cout << "[SDServer] Model unloaded" << std::endl;
}

// ICompletionServer implementation - not supported for image generation
json SDServer::chat_completion(const json& request) {
    return json{
        {"error", {
            {"message", "Image generation models do not support chat completion. Use image generation endpoints instead."},
            {"type", "unsupported_operation"},
            {"code", "model_not_applicable"}
        }}
    };
}

json SDServer::completion(const json& request) {
    return json{
        {"error", {
            {"message", "Image generation models do not support text completion. Use image generation endpoints instead."},
            {"type", "unsupported_operation"},
            {"code", "model_not_applicable"}
        }}
    };
}

json SDServer::responses(const json& request) {
    return json{
        {"error", {
            {"message", "Image generation models do not support responses. Use image generation endpoints instead."},
            {"type", "unsupported_operation"},
            {"code", "model_not_applicable"}
        }}
    };
}

// Image generation helpers
std::string SDServer::generate_output_path() {
    // Generate unique filename
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 999999);

    std::stringstream ss;
    ss << "image_" << std::setfill('0') << std::setw(6) << dis(gen) << ".png";

    return (temp_dir_ / ss.str()).string();
}

std::string SDServer::run_sd_cli(const std::string& prompt,
                                  const std::string& output_path,
                                  int width,
                                  int height,
                                  int steps,
                                  float cfg_scale,
                                  int64_t seed) {
    std::string exe_path = get_sd_executable_path();
    if (exe_path.empty()) {
        throw std::runtime_error("stable-diffusion.cpp executable not found");
    }

    // Build command line arguments for sd CLI
    // Usage: sd -m MODEL -p "PROMPT" -o OUTPUT [OPTIONS]
    std::vector<std::string> args = {
        "-m", model_path_,
        "-p", prompt,
        "-o", output_path,
        "-W", std::to_string(width),
        "-H", std::to_string(height),
        "--steps", std::to_string(steps),
        "--cfg-scale", std::to_string(cfg_scale)
    };

    if (seed >= 0) {
        args.push_back("-s");
        args.push_back(std::to_string(seed));
    }

    if (is_debug()) {
        std::cout << "[SDServer] Running: " << exe_path;
        for (const auto& arg : args) {
            std::cout << " " << arg;
        }
        std::cout << std::endl;
    }

    // Run the CLI synchronously and wait for completion
    auto process_handle = utils::ProcessManager::start_process(
        exe_path,
        args,
        "",     // working_dir (empty = current)
        is_debug()  // inherit_output
    );

    if (process_handle.pid == 0) {
        throw std::runtime_error("Failed to start stable-diffusion.cpp process");
    }

    // Wait for the process to complete
    int exit_code = utils::ProcessManager::wait_for_exit(process_handle, 600000);  // 10 minute timeout

    if (exit_code != 0) {
        throw std::runtime_error("stable-diffusion.cpp exited with code: " + std::to_string(exit_code));
    }

    // Verify output file was created
    if (!fs::exists(output_path)) {
        throw std::runtime_error("Image generation failed: output file not created");
    }

    return output_path;
}

std::string SDServer::read_image_as_base64(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Could not open image file: " + path);
    }

    std::ostringstream oss;
    oss << file.rdbuf();
    std::string binary_data = oss.str();
    file.close();

    // Base64 encode
    static const char base64_chars[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string encoded;
    encoded.reserve(((binary_data.size() + 2) / 3) * 4);

    for (size_t i = 0; i < binary_data.size(); i += 3) {
        unsigned int n = static_cast<unsigned char>(binary_data[i]) << 16;
        if (i + 1 < binary_data.size()) {
            n |= static_cast<unsigned char>(binary_data[i + 1]) << 8;
        }
        if (i + 2 < binary_data.size()) {
            n |= static_cast<unsigned char>(binary_data[i + 2]);
        }

        encoded.push_back(base64_chars[(n >> 18) & 0x3F]);
        encoded.push_back(base64_chars[(n >> 12) & 0x3F]);
        encoded.push_back((i + 1 < binary_data.size()) ? base64_chars[(n >> 6) & 0x3F] : '=');
        encoded.push_back((i + 2 < binary_data.size()) ? base64_chars[n & 0x3F] : '=');
    }

    return encoded;
}

void SDServer::cleanup_temp_file(const std::string& path) {
    try {
        if (fs::exists(path)) {
            fs::remove(path);
            if (is_debug()) {
                std::cout << "[SDServer] Cleaned up temp file: " << path << std::endl;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[SDServer] Warning: Could not delete temp file " << path
                  << ": " << e.what() << std::endl;
    }
}

// IImageServer implementation
json SDServer::image_generations(const json& request) {
    try {
        // Extract parameters from request (OpenAI compatible)
        std::string prompt = request.value("prompt", "");
        if (prompt.empty()) {
            throw std::runtime_error("Missing 'prompt' in request");
        }

        int n = request.value("n", 1);
        if (n < 1 || n > 10) {
            throw std::runtime_error("'n' must be between 1 and 10");
        }

        // Parse size (e.g., "512x512", "1024x1024")
        std::string size = request.value("size", "512x512");
        int width = 512, height = 512;
        size_t x_pos = size.find('x');
        if (x_pos != std::string::npos) {
            try {
                width = std::stoi(size.substr(0, x_pos));
                height = std::stoi(size.substr(x_pos + 1));
            } catch (...) {
                throw std::runtime_error("Invalid size format. Expected 'WIDTHxHEIGHT'");
            }
        }

        // Additional SD-specific parameters (with OpenAI-compatible defaults)
        int steps = request.value("steps", 20);
        float cfg_scale = request.value("cfg_scale", 7.0f);
        int64_t seed = request.value("seed", -1);  // -1 means random

        std::string response_format = request.value("response_format", "b64_json");

        // Generate images
        json data = json::array();
        for (int i = 0; i < n; i++) {
            std::string output_path = generate_output_path();

            try {
                // Run SD CLI
                run_sd_cli(prompt, output_path, width, height, steps, cfg_scale, seed);

                // Read and encode the image
                if (response_format == "b64_json") {
                    std::string base64_image = read_image_as_base64(output_path);
                    data.push_back({{"b64_json", base64_image}});
                } else if (response_format == "url") {
                    // For URL format, we'd need to serve the file somehow
                    // For now, just return the local path as a placeholder
                    data.push_back({{"url", "file://" + output_path}});
                }

                // Clean up temp file if returning base64 and not saving images
                if (response_format == "b64_json" && !save_images_) {
                    cleanup_temp_file(output_path);
                } else if (save_images_) {
                    std::cout << "[SDServer] Image saved to: " << output_path << std::endl;
                }

            } catch (const std::exception& e) {
                if (!save_images_) {
                    cleanup_temp_file(output_path);
                }
                throw;
            }
        }

        // Return OpenAI-compatible response
        auto now = std::chrono::system_clock::now();
        auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(
            now.time_since_epoch()).count();

        return json{
            {"created", timestamp},
            {"data", data}
        };

    } catch (const std::exception& e) {
        return json{
            {"error", {
                {"message", std::string("Image generation failed: ") + e.what()},
                {"type", "image_generation_error"}
            }}
        };
    }
}

} // namespace backends
} // namespace lemon
