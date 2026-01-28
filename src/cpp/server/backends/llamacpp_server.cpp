#include "lemon/backends/llamacpp_server.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/utils/http_client.h"
#include "lemon/utils/process_manager.h"
#include "lemon/utils/path_utils.h"
#include "lemon/utils/json_utils.h"
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
#include <set>

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

// Embedding model batch configuration set to 8192 as default
static const int EMBEDDING_CTX_SIZE = 8192;
static const int EMBEDDING_BATCH_SIZE = 8192;
static const int EMBEDDING_UBATCH_SIZE = 8192;

// Helper to load backend versions from configuration file
static std::string get_llamacpp_version(const std::string& backend) {
    std::string config_path = utils::get_resource_path("resources/backend_versions.json");

    try {
        json config = utils::JsonUtils::load_from_file(config_path);

        if (!config.contains("llamacpp") || !config["llamacpp"].is_object()) {
            throw std::runtime_error("backend_versions.json is missing 'llamacpp' section");
        }

        const auto& llamacpp_config = config["llamacpp"];

        if (!llamacpp_config.contains(backend) || !llamacpp_config[backend].is_string()) {
            throw std::runtime_error("backend_versions.json is missing version for backend: " + backend);
        }

        std::string version = llamacpp_config[backend].get<std::string>();
        std::cout << "[LlamaCpp] Using " << backend << " version from config: " << version << std::endl;
        return version;

    } catch (const std::exception& e) {
        std::cerr << "\n" << std::string(70, '=') << std::endl;
        std::cerr << "ERROR: Failed to load llama.cpp version from configuration" << std::endl;
        std::cerr << std::string(70, '=') << std::endl;
        std::cerr << "\nConfig file: " << config_path << std::endl;
        std::cerr << "Backend: " << backend << std::endl;
        std::cerr << "Error: " << e.what() << std::endl;
        std::cerr << "\nThe backend_versions.json file is required and must contain valid" << std::endl;
        std::cerr << "version information for all llama.cpp backends." << std::endl;
        std::cerr << std::string(70, '=') << std::endl << std::endl;
        throw;
    }
}

// Helper to add a flag-only argument (e.g., --jinja, --embeddings)
static void push_arg(std::vector<std::string>& args,
                    std::set<std::string>& reserved,
                    const std::string& key) {
    args.push_back(key);
    reserved.insert(key);
}

// Helper to add a flag-value pair (e.g., --port 8000, -m model.gguf)
static void push_arg(std::vector<std::string>& args,
                    std::set<std::string>& reserved,
                    const std::string& key,
                    const std::string& value) {
    args.push_back(key);
    args.push_back(value);
    reserved.insert(key);
}

// Helper to add a flag-only overridable argument (e.g., --context-shift)
static void push_overridable_arg(std::vector<std::string>& args,
                    const std::string& custom_args,
                    const std::string& key) {
    // boolean flags in llama-server can be turned off adding the --no- prefix to their name
    auto anti_key = "--no-" + key.substr(2);
    if ((custom_args.find(key) == std::string::npos) && (custom_args.find(anti_key) == std::string::npos)) {
        args.push_back(key);
    }
}

// Helper to add a flag-value overridable pair (e.g., --keep 16)
static void push_overridable_arg(std::vector<std::string>& args,
                    const std::string& custom_args,
                    const std::string& key,
                    const std::string& value) {
    if (custom_args.find(key) == std::string::npos) {
        args.push_back(key);
        args.push_back(value);
    }
}

// Helper to tokenize custom args string into vector
static std::vector<std::string> parse_custom_args(const std::string& custom_args_str) {
    std::vector<std::string> result;
    if (custom_args_str.empty()) {
        return result;
    }

    std::string current_arg;
    bool in_quotes = false;
    char quote_char = '\0';

    for (char c : custom_args_str) {
        if (!in_quotes && (c == '"' || c == '\'')) {
            in_quotes = true;
            quote_char = c;
        } else if (in_quotes && c == quote_char) {
            in_quotes = false;
            quote_char = '\0';
        } else if (!in_quotes && c == ' ') {
            if (!current_arg.empty()) {
                result.push_back(current_arg);
                current_arg.clear();
            }
        } else {
            current_arg += c;
        }
    }

    if (!current_arg.empty()) {
        result.push_back(current_arg);
    }

    return result;
}

// Helper to validate custom arguments don't conflict with reserved flags
static std::string validate_custom_args(const std::string& custom_args_str,
                                       const std::set<std::string>& reserved_flags) {
    std::vector<std::string> custom_args = parse_custom_args(custom_args_str);

    for (const auto& arg : custom_args) {
        // Extract flag name (handle --flag=value format)
        std::string flag = arg;
        size_t eq_pos = flag.find('=');
        if (eq_pos != std::string::npos) {
            flag = flag.substr(0, eq_pos);
        }

        // Check if it's a flag and if it's reserved
        if (!flag.empty() && flag[0] == '-') {
            if (reserved_flags.find(flag) != reserved_flags.end()) {
                // Build error message with all reserved flags
                std::string reserved_list;
                for (const auto& rf : reserved_flags) {
                    if (!reserved_list.empty()) reserved_list += ", ";
                    reserved_list += rf;
                }

                return "Argument '" + flag + "' is managed by Lemonade and cannot be overridden.\n"
                       "Reserved arguments: " + reserved_list;
            }
        }
    }

    return "";  // Valid
}

LlamaCppServer::LlamaCppServer(const std::string& log_level, ModelManager* model_manager)
    : WrappedServer("llama-server", log_level, model_manager) {
}

LlamaCppServer::~LlamaCppServer() {
    unload();
}

// Helper to identify ROCm architecture from system
static std::string identify_rocm_arch() {
    // Try to detect GPU architecture, default to gfx110X on any failure
    try {
        auto system_info = lemon::create_system_info();

        // Check iGPU first
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
    } catch (...) {
        // Detection failed - use default
    }

    return "gfx110X";  // Default architecture
}

// Helper to get the install directory for llama-server binaries
// Policy: Put in llama/{backend}/ next to the executable
static std::string get_install_directory(const std::string& backend) {
    return (fs::path(get_downloaded_bin_dir()) / "llama" / backend).string();
}

void LlamaCppServer::install(const std::string& backend) {
    std::string install_dir;
    std::string version_file;
    std::string backend_file;

    std::string exe_path = find_external_llama_server(backend);
    bool needs_install = exe_path.empty();

    // Get expected version from config file (or fallback to defaults)
    std::string expected_version = get_llamacpp_version(backend);

    if (needs_install) {
        install_dir = get_install_directory(backend);
        version_file = (fs::path(install_dir) / "version.txt").string();
        backend_file = (fs::path(install_dir) / "backend.txt").string();

        // Check if already installed with correct version
        exe_path = find_executable_in_install_dir(install_dir);
        needs_install = exe_path.empty();

        if (!needs_install && fs::exists(version_file) && fs::exists(backend_file)) {
            std::string installed_version, installed_backend;

            // Read version info in a separate scope to ensure files are closed
            {
                std::ifstream vf(version_file);
                std::ifstream bf(backend_file);
                std::getline(vf, installed_version);
                std::getline(bf, installed_backend);
            }  // Files are closed here when ifstream objects go out of scope

            if (installed_version != expected_version || installed_backend != backend) {
                std::cout << "[LlamaCpp] Upgrading from " << installed_version
                        << " to " << expected_version << std::endl;
                needs_install = true;
                fs::remove_all(install_dir);
            }
        }
    }

    if (needs_install) {
        std::cout << "[LlamaCpp] Installing llama-server (backend: " << backend
                 << ", version: " << expected_version << ")" << std::endl;

        // Create install directory
        fs::create_directories(install_dir);

        // Determine download URL
        std::string repo, filename;

        if (backend == "rocm") {
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

        } else if (backend == "metal") {
            // Metal support for macOS Apple Silicon from ggml-org/llama.cpp
            repo = "ggml-org/llama.cpp";
#ifdef __APPLE__
            filename = "llama-" + expected_version + "-bin-macos-arm64.tar.gz";
#else
            throw std::runtime_error("Metal llamacpp only supported on macOS");
#endif

        } else if (backend == "cpu") {
            // CPU-only builds from ggml-org/llama.cpp
            repo = "ggml-org/llama.cpp";

#ifdef _WIN32
            filename = "llama-" + expected_version + "-bin-win-cpu-x64.zip";
#elif defined(__linux__)
            filename = "llama-" + expected_version + "-bin-ubuntu-x64.tar.gz";
#else
            throw std::runtime_error("CPU llamacpp not supported on this platform");
#endif

        } else {  // vulkan
            // Vulkan support from ggml-org/llama.cpp
            repo = "ggml-org/llama.cpp";
#ifdef _WIN32
            filename = "llama-" + expected_version + "-bin-win-vulkan-x64.zip";
#elif defined(__linux__)
            filename = "llama-" + expected_version + "-bin-ubuntu-vulkan-x64.tar.gz";
#else
            throw std::runtime_error("Vulkan llamacpp only supported on Windows and Linux");
#endif
        }

        std::string url = "https://github.com/" + repo + "/releases/download/" +
                         expected_version + "/" + filename;

        // Download archive to HuggingFace cache directory (follows HF conventions)
        fs::path cache_dir = model_manager_ ? model_manager_->get_hf_cache_dir() : "";
        if (cache_dir.empty()) {
            throw std::runtime_error("ModelManager not available for cache directory lookup");
        }
        fs::create_directories(cache_dir);
        std::string archive_path = (cache_dir / filename).string();

        std::cout << "[LlamaCpp] Downloading from: " << url << std::endl;
        std::cout << "[LlamaCpp] Downloading to: " << archive_path << std::endl;

        auto result = utils::HttpClient::download_file(
            url,
            archive_path,
            utils::create_throttled_progress_callback()
        );

        if (!result.success) {
            throw std::runtime_error("Failed to download llama-server: " + result.error_message);
        }

        std::cout << "[LlamaCpp] Download complete!" << std::endl;

        // Verify the downloaded file exists and is valid
        if (!fs::exists(archive_path)) {
            throw std::runtime_error("Downloaded archive file does not exist: " + archive_path);
        }

        std::uintmax_t file_size = fs::file_size(archive_path);
        std::cout << "[LlamaCpp] Downloaded archive file size: " << (file_size / 1024 / 1024) << " MB" << std::endl;

        const std::uintmax_t MIN_ARCHIVE_SIZE = 1024 * 1024;  // 1 MB
        if (file_size < MIN_ARCHIVE_SIZE) {
            std::cerr << "[LlamaCpp] ERROR: Downloaded file is too small (" << file_size << " bytes)" << std::endl;
            std::cerr << "[LlamaCpp] This usually indicates a failed or incomplete download." << std::endl;
            fs::remove(archive_path);
            throw std::runtime_error("Downloaded file is too small (< 1 MB), likely corrupted or incomplete");
        }

        // Extract (handles both .zip and .tar.gz based on extension)
        if (!backends::BackendUtils::extract_archive(archive_path, install_dir, "LlamaCpp")) {
            // Clean up corrupted files
            fs::remove(archive_path);
            fs::remove_all(install_dir);
            throw std::runtime_error("Failed to extract llama-server archive");
        }

        // Verify extraction succeeded by finding the executable
        exe_path = find_executable_in_install_dir(install_dir);
        if (exe_path.empty()) {
            std::cerr << "[LlamaCpp] ERROR: Extraction completed but executable not found in: " << install_dir << std::endl;
            std::cerr << "[LlamaCpp] This usually indicates a corrupted download or unexpected archive structure." << std::endl;
            std::cerr << "[LlamaCpp] Cleaning up..." << std::endl;
            // Clean up corrupted files
            fs::remove(archive_path);
            fs::remove_all(install_dir);
            throw std::runtime_error("Extraction failed: executable not found. Downloaded file may be corrupted.");
        }

        std::cout << "[LlamaCpp] Executable verified at: " << exe_path << std::endl;

        // Save version and backend info
        std::ofstream vf(version_file);
        vf << expected_version;
        vf.close();

        std::ofstream bf(backend_file);
        bf << backend;
        bf.close();

#ifndef _WIN32
        // Make executable on Linux/macOS
        chmod(exe_path.c_str(), 0755);
#endif

        // Delete archive file
        fs::remove(archive_path);

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
                         const ModelInfo& model_info,
                         const RecipeOptions& options,
                         bool do_not_upgrade) {
    std::cout << "[LlamaCpp] Loading model: " << model_name << std::endl;

    // Llamacpp Backend logging
    std::cout << "[LlamaCpp] Per-model settings: " << options.to_log_string() << std::endl;

    int ctx_size = options.get_option("ctx_size");
    std::string llamacpp_backend = options.get_option("llamacpp_backend");
    std::string llamacpp_args = options.get_option("llamacpp_args");

    bool use_gpu = (llamacpp_backend != "cpu");

    // Install llama-server if needed (use per-model backend)
    install(llamacpp_backend);

    // Use pre-resolved GGUF path
    std::string gguf_path = model_info.resolved_path;
    if (gguf_path.empty()) {
        throw std::runtime_error("GGUF file not found for checkpoint: " + model_info.checkpoint);
    }

    std::cout << "[LlamaCpp] Using GGUF: " << gguf_path << std::endl;

    // Get mmproj path for vision models
    std::string mmproj_path;
    if (!model_info.mmproj.empty()) {
        fs::path search_path;

        // For discovered models (from extra_models_dir), search in the model's directory
        if (model_info.source == "extra_models_dir") {
            // checkpoint is the directory path for discovered models
            search_path = fs::path(model_info.checkpoint);
            // If checkpoint is the GGUF file itself (standalone file), use its parent directory
            if (!fs::is_directory(search_path)) {
                search_path = search_path.parent_path();
            }
        } else {
            // For HuggingFace models, use the HF cache directory
            std::string repo_id = model_info.checkpoint;
            size_t colon_pos = model_info.checkpoint.find(':');
            if (colon_pos != std::string::npos) {
                repo_id = model_info.checkpoint.substr(0, colon_pos);
            }

            // Convert org/model to models--org--model
            std::string cache_dir_name = "models--";
            for (char c : repo_id) {
                cache_dir_name += (c == '/') ? "--" : std::string(1, c);
            }

            std::string hf_cache = model_manager_ ? model_manager_->get_hf_cache_dir() : "";
            if (hf_cache.empty()) {
                throw std::runtime_error("ModelManager not available for cache directory lookup");
            }
            search_path = fs::path(hf_cache) / cache_dir_name;
        }

        // Search for mmproj file
        std::cout << "[LlamaCpp] Searching for mmproj '" << model_info.mmproj
                  << "' in: " << search_path << std::endl;

        if (fs::exists(search_path)) {
            try {
                for (const auto& entry : fs::recursive_directory_iterator(search_path)) {
                    if (entry.is_regular_file()) {
                        std::string filename = entry.path().filename().string();
                        if (filename == model_info.mmproj) {
                            mmproj_path = entry.path().string();
                            std::cout << "[LlamaCpp] Found mmproj file: " << mmproj_path << std::endl;
                            break;
                        }
                    }
                }
            } catch (const std::exception& e) {
                std::cerr << "[LlamaCpp] Error during mmproj search: " << e.what() << std::endl;
            }
        } else {
            std::cout << "[LlamaCpp] Search path does not exist: " << search_path << std::endl;
        }

        if (mmproj_path.empty()) {
            std::cout << "[LlamaCpp] Warning: mmproj file '" << model_info.mmproj
                      << "' not found in cache" << std::endl;
        }
    }

    // Choose port
    port_ = choose_port();

    // Get executable path
    std::string executable = get_llama_server_path(llamacpp_backend);

    // Check for embeddings and reranking support based on model type
    bool supports_embeddings = (model_info.type == ModelType::EMBEDDING);
    bool supports_reranking = (model_info.type == ModelType::RERANKING);

    // For embedding models, use a larger context size to support longer individual
    // strings. Embedding requests can include multiple strings in a batch, and each
    // string needs to fit within the context window.
    if (supports_embeddings && ctx_size < EMBEDDING_CTX_SIZE) {
        ctx_size = EMBEDDING_CTX_SIZE;
    }

    // Build command arguments while tracking reserved flags
    std::vector<std::string> args;
    std::set<std::string> reserved_flags;

    push_arg(args, reserved_flags, "-m", gguf_path);
    push_arg(args, reserved_flags, "--ctx-size", std::to_string(ctx_size));
    push_arg(args, reserved_flags, "--port", std::to_string(port_));
    push_arg(args, reserved_flags, "--jinja");

    std::cout << "[LlamaCpp] Using backend: " << llamacpp_backend << "\n"
            << "[LlamaCpp] Use GPU: " << (use_gpu ? "true" : "false") << std::endl;

    // Add mmproj file if present (for vision models)
    if (!mmproj_path.empty()) {
        push_arg(args, reserved_flags, "--mmproj", mmproj_path);
        if (!use_gpu) {
            std::cout << "[LlamaCpp] Skipping mmproj argument since GPU mode is not enabled" << std::endl;
            push_arg(args, reserved_flags, "--no-mmproj-offload");
        }
    }

    // Enable context shift for vulkan/rocm (not supported on Metal)
    if (llamacpp_backend == "vulkan" || llamacpp_backend == "rocm") {
        push_overridable_arg(args, llamacpp_args, "--context-shift");
        push_overridable_arg(args, llamacpp_args, "--keep", "16");
    } else {
        // For Metal, just use keep without context-shift
        push_overridable_arg(args, llamacpp_args, "--keep", "16");
    }

    // Use legacy reasoning formatting
    push_overridable_arg(args, llamacpp_args, "--reasoning-format", "auto");

    // Add embeddings support if the model supports it
    if (supports_embeddings) {
        std::cout << "[LlamaCpp] Model supports embeddings, adding --embeddings flag" << std::endl;
        push_arg(args, reserved_flags, "--embeddings");
    }

    // Add reranking support if the model supports it
    if (supports_reranking) {
        std::cout << "[LlamaCpp] Model supports reranking, adding --reranking flag" << std::endl;
        push_arg(args, reserved_flags, "--reranking");
    }

    // Configure GPU layers
    if (use_gpu) {
        push_arg(args, reserved_flags, "-ngl", "99");  // 99 for GPU, 0 for CPU-only
    } else {
        std::cout << "[LlamaCpp] ngl set to 0" << std::endl;
        push_arg(args, reserved_flags, "-ngl", "0");   // 0
    }

    // Validate and append custom arguments
    if (!llamacpp_args.empty()) {
        std::string validation_error = validate_custom_args(llamacpp_args, reserved_flags);
        if (!validation_error.empty()) {
            throw std::invalid_argument(
                "Invalid custom llama-server arguments:\n" + validation_error
            );
        }

        std::cout << "[LlamaCpp] Adding custom arguments: " << llamacpp_args << std::endl;
        std::vector<std::string> custom_args_vec = parse_custom_args(llamacpp_args);
        args.insert(args.end(), custom_args_vec.begin(), custom_args_vec.end());
    }

    std::cout << "[LlamaCpp] Starting llama-server..." << std::endl;

    // For ROCm on Linux, set LD_LIBRARY_PATH to include the ROCm library directory
    std::vector<std::pair<std::string, std::string>> env_vars;
#ifndef _WIN32
    if (llamacpp_backend == "rocm") {
        // Get the directory containing the executable (where ROCm .so files are)
        fs::path exe_dir = fs::path(executable).parent_path();
        std::string lib_path = exe_dir.string();

        // Preserve existing LD_LIBRARY_PATH if it exists
        const char* existing_ld_path = std::getenv("LD_LIBRARY_PATH");
        if (existing_ld_path && strlen(existing_ld_path) > 0) {
            lib_path = lib_path + ":" + std::string(existing_ld_path);
        }

        env_vars.push_back({"LD_LIBRARY_PATH", lib_path});
        std::cout << "[LlamaCpp] Setting LD_LIBRARY_PATH=" << lib_path << std::endl;
    }
#else
    // For ROCm on Windows with gfx1151, set OCL_SET_SVMSIZE
    // This is a patch to enable loading larger models
    if (llamacpp_backend == "rocm") {
        std::string arch = identify_rocm_arch();
        if (arch == "gfx1151") {
            env_vars.push_back({"OCL_SET_SVM_SIZE", "262144"});
            std::cout << "[LlamaCpp] Setting OCL_SET_SVM_SIZE=262144 for gfx1151 (enables loading larger models)" << std::endl;
        }
    }
#endif

    // Start process (inherit output if debug logging enabled, filter health check spam)
    process_handle_ = ProcessManager::start_process(executable, args, "", is_debug(), true, env_vars);

    // Wait for server to be ready
    if (!wait_for_ready()) {
        ProcessManager::stop_process(process_handle_);
        throw std::runtime_error("llama-server failed to start");
    }

    std::cout << "[LlamaCpp] Model loaded on port " << port_ << std::endl;
}

void LlamaCppServer::unload() {
    std::cout << "[LlamaCpp] Unloading model..." << std::endl;
#ifdef _WIN32
    if (process_handle_.handle) {
#else
    if (process_handle_.pid > 0) {
#endif
        ProcessManager::stop_process(process_handle_);
        process_handle_ = {nullptr, 0};
        port_ = 0;
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

std::string LlamaCppServer::find_executable_in_install_dir(const std::string& install_dir) {
    // Try multiple possible locations where llama-server might be extracted
    std::vector<std::string> possible_paths;

#ifdef _WIN32
    // Windows: only one location
    possible_paths.push_back((fs::path(install_dir) / "llama-server.exe").string());
#else
    // Linux/macOS: try multiple locations in order of likelihood
    // 1. Official llama.cpp releases extract to build/bin/
    possible_paths.push_back((fs::path(install_dir) / "build" / "bin" / "llama-server").string());

    // 2. ROCm builds may extract to root
    possible_paths.push_back((fs::path(install_dir) / "llama-server").string());

    // 3. Some builds extract to bin/
    possible_paths.push_back((fs::path(install_dir) / "bin" / "llama-server").string());
#endif

    // Check each path and return the first one that exists
    for (const auto& path : possible_paths) {
        if (fs::exists(path)) {
            return path;
        }
    }

    // Not found in any expected location
    return "";
}

std::string LlamaCppServer::find_external_llama_server(const std::string& backend) {
    std::string upper_backend = backend;
    std::transform(upper_backend.begin(), upper_backend.end(), upper_backend.begin(), ::toupper);
    std::string env = "LEMONADE_LLAMACPP_" + upper_backend + "_BIN";
    const char* llama_bin_env = std::getenv(env.c_str());
    if (!llama_bin_env) {
        return "";
    }

    std::string llama_bin = std::string(llama_bin_env);

    return fs::exists(llama_bin) ? llama_bin : "";
}

std::string LlamaCppServer::get_llama_server_path(const std::string& backend) {
    std::string exe_path = find_external_llama_server(backend);

    if (!exe_path.empty()) {
        return exe_path;
    }

    std::string install_dir = get_install_directory(backend);
    exe_path = find_executable_in_install_dir(install_dir);

    if (!exe_path.empty()) {
        return exe_path;
    }

    // If not found, throw error with helpful message
    throw std::runtime_error("llama-server not found in install directory: " + install_dir +
                           "\nExpected locations checked: " +
                           "\n  - " + install_dir + "/llama-server.exe (Windows)" +
                           "\n  - " + install_dir + "/build/bin/llama-server (official releases)" +
                           "\n  - " + install_dir + "/llama-server (ROCm/custom builds)" +
                           "\n  - " + install_dir + "/bin/llama-server" +
                           "\nThis may indicate a failed installation or corrupted download.");
}

} // namespace backends
} // namespace lemon
