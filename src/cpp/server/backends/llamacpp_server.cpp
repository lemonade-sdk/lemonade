#include "lemon/backends/llamacpp_server.h"
#include "lemon/utils/http_client.h"
#include "lemon/utils/process_manager.h"
#include "lemon/error_types.h"
#include "lemon/system_info.h"
#include <iostream>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <regex>
#include <thread>
#include <chrono>
#include <algorithm>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;
using namespace lemon::utils;

namespace lemon {
namespace backends {

// llama.cpp version constants
static const std::string LLAMA_VERSION_VULKAN = "b6510";
static const std::string LLAMA_VERSION_ROCM = "b1066";
static const std::string LLAMA_VERSION_METAL = "b6510";

LlamaCppServer::LlamaCppServer(const std::string& backend, const std::string& log_level)
    : WrappedServer("llama-server", log_level), backend_(backend) {
}

LlamaCppServer::~LlamaCppServer() {
    unload();
}

// Helper to identify ROCm architecture from GPU name
static std::string identify_rocm_arch_from_name(const std::string& device_name) {
    std::string device_lower = device_name;
    std::transform(device_lower.begin(), device_lower.end(), device_lower.begin(), ::tolower);
    
    if (device_lower.find("radeon") == std::string::npos) {
        return "";
    }
    
    // STX Halo iGPUs (gfx1151 architecture)
    // Radeon 8050S Graphics / Radeon 8060S Graphics
    if (device_lower.find("8050s") != std::string::npos || 
        device_lower.find("8060s") != std::string::npos) {
        return "gfx1151";
    }
    
    // RDNA4 GPUs (gfx120X architecture)
    // AMD Radeon AI PRO R9700, AMD Radeon RX 9070 XT, AMD Radeon RX 9070 GRE,
    // AMD Radeon RX 9070, AMD Radeon RX 9060 XT
    if (device_lower.find("r9700") != std::string::npos ||
        device_lower.find("9060") != std::string::npos ||
        device_lower.find("9070") != std::string::npos) {
        return "gfx120X";
    }
    
    // RDNA3 GPUs (gfx110X architecture)
    // AMD Radeon PRO V710, AMD Radeon PRO W7900 Dual Slot, AMD Radeon PRO W7900,
    // AMD Radeon PRO W7800 48GB, AMD Radeon PRO W7800, AMD Radeon PRO W7700,
    // AMD Radeon RX 7900 XTX, AMD Radeon RX 7900 XT, AMD Radeon RX 7900 GRE,
    // AMD Radeon RX 7800 XT, AMD Radeon RX 7700 XT
    if (device_lower.find("7700") != std::string::npos ||
        device_lower.find("7800") != std::string::npos ||
        device_lower.find("7900") != std::string::npos ||
        device_lower.find("v710") != std::string::npos) {
        return "gfx110X";
    }
    
    return "";
}

// Helper to identify ROCm architecture from system
static std::string identify_rocm_arch() {
    auto system_info = lemon::create_system_info();
    
    // Check iGPU
    auto igpu = system_info->get_amd_igpu_device();
    if (igpu.available && !igpu.name.empty()) {
        std::string arch = identify_rocm_arch_from_name(igpu.name);
        if (!arch.empty()) {
            return arch;
        }
    }
    
    // Check dGPUs
    auto dgpus = system_info->get_amd_dgpu_devices();
    for (const auto& gpu : dgpus) {
        if (gpu.available && !gpu.name.empty()) {
            std::string arch = identify_rocm_arch_from_name(gpu.name);
            if (!arch.empty()) {
                return arch;
            }
        }
    }
    
    // Default to gfx110X if no specific arch detected
    return "gfx110X";
}

// Helper to get the cache directory
static std::string get_cache_dir() {
    // Check environment variable first
    const char* cache_env = std::getenv("LEMONADE_CACHE_DIR");
    if (cache_env) {
        return std::string(cache_env);
    }
    
    // Default cache directory
#ifdef _WIN32
    const char* userprofile = std::getenv("USERPROFILE");
    if (userprofile) {
        return std::string(userprofile) + "\\.cache\\lemonade";
    }
    return "C:\\.cache\\lemonade";
#else
    const char* home = std::getenv("HOME");
    if (home) {
        return std::string(home) + "/.cache/lemonade";
    }
    return "/tmp/.cache/lemonade";
#endif
}

// Helper to get the install directory for llama-server
// Matches Python: {executable_dir}/{backend}/llama_server/
static std::string get_install_directory(const std::string& backend) {
#ifdef _WIN32
    char exe_path[MAX_PATH];
    GetModuleFileNameA(NULL, exe_path, MAX_PATH);
    fs::path exe_dir = fs::path(exe_path).parent_path();
#else
    fs::path exe_dir = fs::current_path();
#endif
    
    return (exe_dir / backend / "llama_server").string();
}

// Helper to extract ZIP files (Windows/Linux built-in tools)
static bool extract_zip(const std::string& zip_path, const std::string& dest_dir) {
#ifdef _WIN32
    std::cout << "[LlamaCpp] Extracting ZIP to " << dest_dir << std::endl;
    
    // Use PowerShell to extract with error handling
    // Add -ErrorAction Stop to ensure errors are properly caught
    std::string command = "powershell -Command \"try { Expand-Archive -Path '" + 
                         zip_path + "' -DestinationPath '" + dest_dir + 
                         "' -Force -ErrorAction Stop; exit 0 } catch { Write-Error $_.Exception.Message; exit 1 }\"";
    
    int result = system(command.c_str());
    if (result != 0) {
        std::cerr << "[LlamaCpp] PowerShell extraction failed with code: " << result << std::endl;
        return false;
    }
    return true;
#else
    std::cout << "[LlamaCpp] Extracting ZIP to " << dest_dir << std::endl;
    std::string command = "unzip -o \"" + zip_path + "\" -d \"" + dest_dir + "\"";
    int result = system(command.c_str());
    return result == 0;
#endif
}

void LlamaCppServer::install(const std::string& backend) {
    std::string install_dir = get_install_directory(backend_.empty() ? backend : backend_);
    std::string version_file = (fs::path(install_dir) / "version.txt").string();
    std::string backend_file = (fs::path(install_dir) / "backend.txt").string();
    std::string exe_path = (fs::path(install_dir) / "llama-server.exe").string();
    
#ifndef _WIN32
    // On Linux, check build/bin subdirectory
    std::string linux_exe = (fs::path(install_dir) / "build" / "bin" / "llama-server").string();
    if (fs::exists(linux_exe)) {
        exe_path = linux_exe;
    } else {
        exe_path = (fs::path(install_dir) / "llama-server").string();
    }
#endif
    
    // Get expected version
    std::string expected_version;
    if (backend_ == "rocm") {
        expected_version = LLAMA_VERSION_ROCM;
    } else if (backend_ == "metal") {
        expected_version = LLAMA_VERSION_METAL;
    } else {
        expected_version = LLAMA_VERSION_VULKAN;
    }
    
    // Check if already installed with correct version
    bool needs_install = !fs::exists(exe_path);
    
    if (!needs_install && fs::exists(version_file) && fs::exists(backend_file)) {
        std::ifstream vf(version_file);
        std::ifstream bf(backend_file);
        std::string installed_version, installed_backend;
        std::getline(vf, installed_version);
        std::getline(bf, installed_backend);
        
        if (installed_version != expected_version || installed_backend != backend_) {
            std::cout << "[LlamaCpp] Upgrading from " << installed_version 
                     << " to " << expected_version << std::endl;
            needs_install = true;
            fs::remove_all(install_dir);
        }
    }
    
    if (needs_install) {
        std::cout << "[LlamaCpp] Installing llama-server (backend: " << backend_ 
                 << ", version: " << expected_version << ")" << std::endl;
        
        // Create install directory
        fs::create_directories(install_dir);
        
        // Determine download URL
        std::string repo, filename;
        
        if (backend_ == "rocm") {
            // ROCm support from lemonade-sdk/llamacpp-rocm
            repo = "lemonade-sdk/llamacpp-rocm";
            std::string target_arch = identify_rocm_arch();
            
#ifdef _WIN32
            filename = "llama-" + expected_version + "-windows-rocm-" + target_arch + "-x64.zip";
#elif defined(__linux__)
            filename = "llama-" + expected_version + "-ubuntu-rocm-" + target_arch + "-x64.zip";
#else
            throw std::runtime_error("ROCm llamacpp only supported on Windows and Linux");
#endif
            std::cout << "[LlamaCpp] Detected ROCm architecture: " << target_arch << std::endl;
            
        } else if (backend_ == "metal") {
            // Metal support for macOS Apple Silicon from ggml-org/llama.cpp
            repo = "ggml-org/llama.cpp";
#ifdef __APPLE__
            filename = "llama-" + expected_version + "-bin-macos-arm64.zip";
#else
            throw std::runtime_error("Metal llamacpp only supported on macOS");
#endif
            
        } else {  // vulkan
            // Vulkan support from ggml-org/llama.cpp
            repo = "ggml-org/llama.cpp";
#ifdef _WIN32
            filename = "llama-" + expected_version + "-bin-win-vulkan-x64.zip";
#elif defined(__linux__)
            filename = "llama-" + expected_version + "-bin-ubuntu-vulkan-x64.zip";
#else
            throw std::runtime_error("Vulkan llamacpp only supported on Windows and Linux");
#endif
        }
        
        std::string url = "https://github.com/" + repo + "/releases/download/" + 
                         expected_version + "/" + filename;
        
        // Download ZIP right next to the .exe
#ifdef _WIN32
        char exe_path[MAX_PATH];
        GetModuleFileNameA(NULL, exe_path, MAX_PATH);
        fs::path exe_dir = fs::path(exe_path).parent_path();
#else
        fs::path exe_dir = fs::current_path();
#endif
        std::string zip_path = (exe_dir / filename).string();
        
        std::cout << "[LlamaCpp] Downloading from: " << url << std::endl;
        std::cout << "[LlamaCpp] Downloading to: " << zip_path << std::endl;
        
        // Download the file with throttled progress updates (once per second)
        bool download_success = utils::HttpClient::download_file(
            url, 
            zip_path, 
            utils::create_throttled_progress_callback()
        );
        
        if (!download_success) {
            throw std::runtime_error("Failed to download llama-server from: " + url);
        }
        
        std::cout << std::endl << "[LlamaCpp] Download complete!" << std::endl;
        
        // Verify the downloaded file exists and is valid
        if (!fs::exists(zip_path)) {
            throw std::runtime_error("Downloaded ZIP file does not exist: " + zip_path);
        }
        
        std::uintmax_t file_size = fs::file_size(zip_path);
        std::cout << "[LlamaCpp] Downloaded ZIP file size: " << (file_size / 1024 / 1024) << " MB" << std::endl;
        
        const std::uintmax_t MIN_ZIP_SIZE = 1024 * 1024;  // 1 MB
        if (file_size < MIN_ZIP_SIZE) {
            std::cerr << "[LlamaCpp] ERROR: Downloaded file is too small (" << file_size << " bytes)" << std::endl;
            std::cerr << "[LlamaCpp] This usually indicates a failed or incomplete download." << std::endl;
            fs::remove(zip_path);
            throw std::runtime_error("Downloaded file is too small (< 1 MB), likely corrupted or incomplete");
        }
        
        // Extract
        if (!extract_zip(zip_path, install_dir)) {
            // Clean up corrupted files
            fs::remove(zip_path);
            fs::remove_all(install_dir);
            throw std::runtime_error("Failed to extract llama-server archive");
        }
        
        // Verify extraction succeeded by checking if executable exists
        if (!fs::exists(exe_path)) {
            std::cerr << "[LlamaCpp] ERROR: Extraction completed but executable not found at: " << exe_path << std::endl;
            std::cerr << "[LlamaCpp] This usually indicates a corrupted download. Cleaning up..." << std::endl;
            // Clean up corrupted files
            fs::remove(zip_path);
            fs::remove_all(install_dir);
            throw std::runtime_error("Extraction failed: executable not found. Downloaded file may be corrupted.");
        }
        
        std::cout << "[LlamaCpp] Executable verified at: " << exe_path << std::endl;
        
        // Save version and backend info
        std::ofstream vf(version_file);
        vf << expected_version;
        vf.close();
        
        std::ofstream bf(backend_file);
        bf << backend_;
        bf.close();
        
#ifndef _WIN32
        // Make executable on Linux/macOS
        chmod(exe_path.c_str(), 0755);
#endif
        
        // Delete ZIP file
        fs::remove(zip_path);
        
        std::cout << "[LlamaCpp] Installation complete!" << std::endl;
    } else {
        std::cout << "[LlamaCpp] Found llama-server at: " << exe_path << std::endl;
    }
}

std::string LlamaCppServer::download_model(const std::string& checkpoint,
                                          const std::string& mmproj,
                                          bool do_not_upgrade) {
    // Model download is handled by ModelManager
    return checkpoint;
}

void LlamaCppServer::load(const std::string& model_name,
                         const std::string& checkpoint,
                         const std::string& mmproj,
                         int ctx_size,
                         bool do_not_upgrade,
                         const std::vector<std::string>& labels) {
    
    std::cout << "[LlamaCpp] Loading model: " << model_name << std::endl;
    
    // Install llama-server if needed
    install(backend_);
    
    // Find GGUF file
    std::string gguf_path = find_gguf_file(checkpoint);
    if (gguf_path.empty()) {
        throw std::runtime_error("GGUF file not found for checkpoint: " + checkpoint);
    }
    
    std::cout << "[LlamaCpp] Using GGUF: " << gguf_path << std::endl;
    
    // Choose port
    port_ = choose_port();
    std::cout << "llama-server will use port: " << port_ << std::endl;
    
    // Get executable path
    std::string executable = get_llama_server_path();
    
    // Build command arguments to match Python implementation EXACTLY
    std::vector<std::string> args = {
        "-m", gguf_path,
        "--ctx-size", std::to_string(ctx_size),
        "--port", std::to_string(port_),
        "--jinja"  // Enable tool use
    };
    
    // Enable context shift for vulkan/rocm (not supported on Metal)
    if (backend_ == "vulkan" || backend_ == "rocm") {
        args.push_back("--context-shift");
        args.push_back("--keep");
        args.push_back("16");
    } else {
        // For Metal, just use keep without context-shift
        args.push_back("--keep");
        args.push_back("16");
    }
    
    // Use legacy reasoning formatting
    args.push_back("--reasoning-format");
    args.push_back("auto");
    
    // Check for embeddings and reranking support based on labels
    bool supports_embeddings = std::find(labels.begin(), labels.end(), "embeddings") != labels.end();
    bool supports_reranking = std::find(labels.begin(), labels.end(), "reranking") != labels.end();
    
    if (supports_embeddings) {
        std::cout << "[LlamaCpp] Model supports embeddings, adding --embeddings flag" << std::endl;
        args.push_back("--embeddings");
    }
    
    if (supports_reranking) {
        std::cout << "[LlamaCpp] Model supports reranking, adding --reranking flag" << std::endl;
        args.push_back("--reranking");
    }
    
    // Configure GPU layers
    args.push_back("-ngl");
    args.push_back("99");  // 99 for GPU, 0 for CPU-only
    
    std::cout << "[LlamaCpp] Starting llama-server..." << std::endl;
    
    // Start process (inherit output if debug logging enabled)
    process_handle_ = ProcessManager::start_process(executable, args, "", is_debug());
    
    // Wait for server to be ready
    if (!wait_for_ready()) {
        ProcessManager::stop_process(process_handle_);
        throw std::runtime_error("llama-server failed to start");
    }
    
    std::cout << "[LlamaCpp] Model loaded on port " << port_ << std::endl;
    model_path_ = gguf_path;
}

void LlamaCppServer::unload() {
    std::cout << "[LlamaCpp] Unloading model..." << std::endl;
    if (process_handle_.handle) {
        ProcessManager::stop_process(process_handle_);
        process_handle_ = {nullptr, 0};
        port_ = 0;
        model_path_.clear();
    }
}

json LlamaCppServer::chat_completion(const json& request) {
    return forward_request("/v1/chat/completions", request);
}

json LlamaCppServer::completion(const json& request) {
    return forward_request("/v1/completions", request);
}

json LlamaCppServer::embeddings(const json& request) {
    return forward_request("/v1/embeddings", request);
}

json LlamaCppServer::reranking(const json& request) {
    return forward_request("/v1/rerank", request);
}

json LlamaCppServer::responses(const json& request) {
    // Responses API is not supported for llamacpp backend
    return ErrorResponse::from_exception(
        UnsupportedOperationException("Responses API", "llamacpp")
    );
}

void LlamaCppServer::parse_telemetry(const std::string& line) {
    // Parse prompt evaluation
    std::regex prompt_regex(R"(prompt eval time\s*=\s*([\d.]+)\s*ms\s*/\s*(\d+)\s*tokens.*?([\d.]+)\s*tokens per second)");
    std::smatch match;
    
    if (std::regex_search(line, match, prompt_regex)) {
        float prompt_time_ms = std::stof(match[1]);
        int input_tokens = std::stoi(match[2]);
        
        telemetry_.input_tokens = input_tokens;
        telemetry_.time_to_first_token = prompt_time_ms / 1000.0;
        return;
    }
    
    // Parse generation evaluation
    std::regex eval_regex(R"(eval time\s*=\s*([\d.]+)\s*ms\s*/\s*(\d+)\s*tokens.*?([\d.]+)\s*tokens per second)");
    
    if (std::regex_search(line, match, eval_regex)) {
        float eval_time_ms = std::stof(match[1]);
        int output_tokens = std::stoi(match[2]);
        float tokens_per_second = std::stof(match[3]);
        
        telemetry_.output_tokens = output_tokens;
        telemetry_.tokens_per_second = tokens_per_second;
    }
}

std::string LlamaCppServer::get_llama_server_path() {
    // Check our install directory first
    std::string install_dir = get_install_directory(backend_);
    
#ifdef _WIN32
    std::string installed_path = (fs::path(install_dir) / "llama-server.exe").string();
#else
    // On Linux, check build/bin subdirectory first
    std::string installed_path = (fs::path(install_dir) / "build" / "bin" / "llama-server").string();
    if (!fs::exists(installed_path)) {
        installed_path = (fs::path(install_dir) / "llama-server").string();
    }
#endif
    
    if (fs::exists(installed_path)) {
        return installed_path;
    }
    
    // Fallback: check common installation paths
#ifdef _WIN32
    std::vector<std::string> paths = {
        "llama-server.exe",
        "C:\\Program Files\\llama.cpp\\llama-server.exe",
        "C:\\llama.cpp\\llama-server.exe"
    };
#else
    std::vector<std::string> paths = {
        "llama-server",
        "/usr/local/bin/llama-server",
        "/usr/bin/llama-server",
        "~/.local/bin/llama-server"
    };
#endif
    
    for (const auto& path : paths) {
        if (fs::exists(path)) {
            return path;
        }
    }
    
    // Return executable name to try PATH (will fail in start_process with good error)
    return "llama-server";
}

std::string LlamaCppServer::find_gguf_file(const std::string& checkpoint) {
    // Parse checkpoint (format: repo_id:variant or just repo_id)
    std::string repo_id = checkpoint;
    std::string variant;
    
    size_t colon_pos = checkpoint.find(':');
    if (colon_pos != std::string::npos) {
        repo_id = checkpoint.substr(0, colon_pos);
        variant = checkpoint.substr(colon_pos + 1);
    }
    
    std::cout << "[LlamaCpp] Looking for GGUF file - repo_id: " << repo_id << ", variant: " << variant << std::endl;
    
    // Get HF cache directory
    std::string hf_cache;
    const char* hf_home_env = std::getenv("HF_HOME");
    if (hf_home_env) {
        hf_cache = std::string(hf_home_env) + "/hub";
    } else {
#ifdef _WIN32
        const char* userprofile = std::getenv("USERPROFILE");
        if (userprofile) {
            hf_cache = std::string(userprofile) + "\\.cache\\huggingface\\hub";
        }
#else
        const char* home = std::getenv("HOME");
        if (home) {
            hf_cache = std::string(home) + "/.cache/huggingface/hub";
        }
#endif
    }
    
    // Convert repo_id to cache directory name
    std::string cache_dir_name = "models--";
    for (char c : repo_id) {
        if (c == '/') {
            cache_dir_name += "--";
        } else {
            cache_dir_name += c;
        }
    }
    
    std::string model_cache_path = hf_cache + "/" + cache_dir_name;
    std::cout << "[LlamaCpp] Searching in: " << model_cache_path << std::endl;
    
    // Find GGUF file matching variant
    if (fs::exists(model_cache_path)) {
        std::cout << "[LlamaCpp] Cache directory exists, searching for .gguf files..." << std::endl;
        
        for (const auto& entry : fs::recursive_directory_iterator(model_cache_path)) {
            if (entry.is_regular_file()) {
                std::string filename = entry.path().filename().string();
                if (filename.find(".gguf") != std::string::npos) {
                    std::cout << "[LlamaCpp] Found GGUF: " << filename << std::endl;
                    
                    if (variant.empty() || filename.find(variant) != std::string::npos) {
                        std::cout << "[LlamaCpp] Matches variant! Using: " << entry.path().string() << std::endl;
                        return entry.path().string();
                    } else {
                        std::cout << "[LlamaCpp] Doesn't match variant '" << variant << "'" << std::endl;
                    }
                }
            }
        }
        
        std::cout << "[LlamaCpp] No matching GGUF file found" << std::endl;
    } else {
        std::cout << "[LlamaCpp] Cache directory does not exist: " << model_cache_path << std::endl;
    }
    
    return "";
}

} // namespace backends
} // namespace lemon

