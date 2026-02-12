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
#elif defined(__APPLE__)
    #include <mach-o/dyld.h>
    #include <limits.h>
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

// Helper to push reserved flags and their aliases
static void push_reserved(std::set<std::string>& reserved,
                    const std::string& key,
                    const std::vector<std::string>& aliases) {
    reserved.insert(key);
    reserved.insert(aliases.begin(), aliases.end());
}

// Helper to add a flag-only argument (e.g., --jinja, --embeddings)
static void push_arg(std::vector<std::string>& args,
                    std::set<std::string>& reserved,
                    const std::string& key,
                    const std::vector<std::string>& aliases = {}) {
    args.push_back(key);
    push_reserved(reserved, key, aliases);
}

// Helper to add a flag-value pair (e.g., --port 8000, -m model.gguf)
static void push_arg(std::vector<std::string>& args,
                    std::set<std::string>& reserved,
                    const std::string& key,
                    const std::string& value,
                    const std::vector<std::string>& aliases = {}) {
    args.push_back(key);
    args.push_back(value);
    push_reserved(reserved, key, aliases);
}

// Helper to add a flag-only overridable argument (e.g., --context-shift)
static void push_overridable_arg(std::vector<std::string>& args,
                    const std::string& custom_args,
                    const std::string& key) {
    // boolean flags in llama-server can be turned off adding the --no- prefix to their name
    std::string anti_key;
    if (key.rfind("--no-", 0) == 0) {
        anti_key = "--" + key.substr(5); // remove --no- prefix
    } else {
        anti_key = "--no-" + key.substr(2); //remove -- prefix
    }

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

void LlamaCppServer::install(const std::string& backend) {
    std::string repo;
    std::string filename;
    std::string expected_version = BackendUtils::get_backend_version(SPEC.recipe, backend);

    if (backend == "rocm") {
        // ROCm support from lemonade-sdk/llamacpp-rocm
        repo = "lemonade-sdk/llamacpp-rocm";
        std::string target_arch = lemon::SystemInfo::get_rocm_arch();

        if (target_arch.empty()) {
            throw std::runtime_error(
                lemon::SystemInfo::get_unsupported_backend_error("llamacpp", "rocm")
            );
        }

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

    BackendUtils::install_from_github(SPEC, expected_version, repo, filename, backend);
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
    std::string gguf_path = model_info.resolved_path();
    if (gguf_path.empty()) {
        throw std::runtime_error("GGUF file not found for checkpoint: " + model_info.checkpoint());
    }

    std::cout << "[LlamaCpp] Using GGUF: " << gguf_path << std::endl;

    // Get mmproj path for vision models
    std::string mmproj_path = model_info.resolved_path("mmproj");

    // Choose port
    port_ = choose_port();

    // Get executable path
    std::string executable = BackendUtils::get_backend_binary_path(SPEC, llamacpp_backend);

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

    push_arg(args, reserved_flags, "-m", gguf_path, std::vector<std::string>{"--model"});
    push_arg(args, reserved_flags, "--ctx-size", std::to_string(ctx_size), std::vector<std::string>{"-c"});
    push_arg(args, reserved_flags, "--port", std::to_string(port_));
    push_arg(args, reserved_flags, "--jinja", std::vector<std::string>{"--no-jinja"});

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
    push_reserved(reserved_flags, "--mmproj", std::vector<std::string>{"-mm", "-mmu", "--mmproj-url", "--no-mmproj", "--mmproj-auto", "--no-mmproj-auto", "--mmproj-offload", "--no-mmproj-offload"});

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

    // Disable llamacpp webui by default
    push_overridable_arg(args, llamacpp_args, "--no-webui");

    // Add embeddings support if the model supports it
    if (supports_embeddings) {
        std::cout << "[LlamaCpp] Model supports embeddings, adding --embeddings flag" << std::endl;
        push_arg(args, reserved_flags, "--embeddings");
    }
    push_reserved(reserved_flags, "--embeddings", std::vector<std::string>{"--embedding"});

    // Add reranking support if the model supports it
    if (supports_reranking) {
        std::cout << "[LlamaCpp] Model supports reranking, adding --reranking flag" << std::endl;
        push_arg(args, reserved_flags, "--reranking");
    }
    push_reserved(reserved_flags, "--reranking", std::vector<std::string>{"--rerank"});

    // Configure GPU layers
    std::string gpu_layers = use_gpu ? "99" : "0";  // 99 for GPU, 0 for CPU-only
    std::cout << "[LlamaCpp] ngl set to " << gpu_layers << std::endl;
    push_arg(args, reserved_flags, "-ngl", gpu_layers, std::vector<std::string>{"--gpu-layers", "--n-gpu-layers"});

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
        std::string arch = lemon::SystemInfo::get_rocm_arch();
        if (arch == "gfx1151") {
            env_vars.push_back({"OCL_SET_SVM_SIZE", "262144"});
            std::cout << "[LlamaCpp] Setting OCL_SET_SVM_SIZE=262144 for gfx1151 (enables loading larger models)" << std::endl;
        }
    }
#endif

    // Start process (inherit output if debug logging enabled, filter health check spam)
    process_handle_ = ProcessManager::start_process(executable, args, "", is_debug(), true, env_vars);

    // Wait for server to be ready
    if (!wait_for_ready("/health")) {
        ProcessManager::stop_process(process_handle_);
        process_handle_ = {nullptr, 0};  // Reset to prevent double-stop on destructor
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
    // OpenAI API compatibility: Transform max_completion_tokens to max_tokens
    // OpenAI deprecated max_tokens in favor of max_completion_tokens (Sep 2024)
    // but llama.cpp only supports the older max_tokens parameter
    json modified_request = request;
    if (modified_request.contains("max_completion_tokens") && !modified_request.contains("max_tokens")) {
        modified_request["max_tokens"] = modified_request["max_completion_tokens"];
    }
    return forward_request("/v1/chat/completions", modified_request);
}

json LlamaCppServer::completion(const json& request) {
    // OpenAI API compatibility: Transform max_completion_tokens to max_tokens
    // OpenAI deprecated max_tokens in favor of max_completion_tokens (Sep 2024)
    // but llama.cpp only supports the older max_tokens parameter
    json modified_request = request;
    if (modified_request.contains("max_completion_tokens") && !modified_request.contains("max_tokens")) {
        modified_request["max_tokens"] = modified_request["max_completion_tokens"];
    }
    return forward_request("/v1/completions", modified_request);
}

json LlamaCppServer::embeddings(const json& request) {
    return forward_request("/v1/embeddings", request);
}

json LlamaCppServer::reranking(const json& request) {
    return forward_request("/v1/rerank", request);
}

json LlamaCppServer::responses(const json& request) {
    return forward_request("/v1/responses", request);
}

} // namespace backends
} // namespace lemon
