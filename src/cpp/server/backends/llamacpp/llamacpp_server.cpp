#include "lemon/backends/llamacpp/llamacpp_server.h"
#include "lemon/backends/llamacpp/llamacpp.h"
#include "lemon/backends/llamacpp/llamacpp_gguf.h"
#include "lemon/backends/llamacpp/llamacpp_request.h"
#include "llamacpp_system_utils.h"
#include "lemon/backends/backend_registry.h"
#include "lemon/backends/backend_ops.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/gguf_capabilities.h"
#include "lemon/gguf_reader.h"
#include "lemon/model_manager.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <regex>
#include <system_error>
#include <utility>
#include "lemon/auto_tune.h"
#include "lemon/backend_manager.h"
#include "lemon/runtime_config.h"
#include "lemon/utils/custom_args.h"
#include "lemon/utils/recipe_arg_resolver.h"
#include "lemon/utils/process_manager.h"
#include "lemon/utils/json_utils.h"
#include "lemon/utils/path_utils.h"
#include "lemon/error_types.h"
#include "lemon/system_info.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <lemon/utils/aixlog.hpp>
#include <set>
#ifdef __APPLE__
#include <pwd.h>
#include <unistd.h>
#endif

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

// Helper to add a flag-value pair (e.g., --port 13305, -m model.gguf)
static void push_arg(std::vector<std::string>& args,
                    std::set<std::string>& reserved,
                    const std::string& key,
                    const std::string& value,
                    const std::vector<std::string>& aliases = {}) {
    args.push_back(key);
    args.push_back(value);
    push_reserved(reserved, key, aliases);
}

static std::string resolve_llamacpp_backend(const std::string& backend) {
    if (backend == "rocm") {
        // Map "rocm" to the appropriate channel based on config
        std::string channel = "stable";  // default to stable for now
        if (auto* cfg = RuntimeConfig::global()) {
            channel = cfg->rocm_channel();
        }
        return "rocm-" + channel;
    }
    return backend;
}

static bool is_llamacpp_rocm_backend(const std::string& backend) {
    return backend == "rocm-stable" || backend == "rocm-nightly";
}

static bool is_llamacpp_cuda_backend(const std::string& backend) {
    return backend == "cuda";
}

static bool is_dflash_draft_checkpoint(std::string checkpoint) {
    std::transform(checkpoint.begin(), checkpoint.end(), checkpoint.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    size_t separator = checkpoint.find_last_of("/:\\");
    std::string filename = separator == std::string::npos
                               ? checkpoint
                               : checkpoint.substr(separator + 1);
    return filename.rfind("dflash-", 0) == 0 || filename == "dflash.gguf";
}

static std::string resolve_llamacpp_runtime_args(const ModelInfo& model_info,
                                                 const std::string& custom_args,
                                                 bool merge_args,
                                                 long sleep_idle_seconds = -1) {
    // Soft-idle downsize (see LlamaCppOps::resolve_runtime_options) must be applied
    // even when merge_args=false, otherwise a user opting out of Lemonade's other
    // runtime defaults also silently disables auto_evict's downsize behavior.
    // llama-server rejects 0 (valid range is -1=disabled or >=1), so a
    // downsize_idle_timeout of 0 ("downsize as soon as idle") maps to the
    // smallest valid finite value instead of being passed through verbatim.
    std::string args = custom_args;
    if (sleep_idle_seconds >= 0) {
        args = append_runtime_arg_defaults(
            args,
            {{"--sleep-idle-seconds " + std::to_string(std::max(1L, sleep_idle_seconds)),
              "--sleep-idle-seconds"}});
    }

    if (!merge_args) return args;

    std::vector<RuntimeArgDefault> defaults;

    const std::string draft_checkpoint = model_info.checkpoint("draft");
    const bool has_dflash_label =
        std::find(model_info.labels.begin(), model_info.labels.end(), "dflash") !=
        model_info.labels.end();
    const bool is_dflash_draft =
        !draft_checkpoint.empty() && is_dflash_draft_checkpoint(draft_checkpoint);
    const bool uses_mtp =
        std::find(model_info.labels.begin(), model_info.labels.end(), "mtp") !=
        model_info.labels.end();

    if (is_dflash_draft && has_dflash_label) {
        defaults.push_back({"--spec-type draft-dflash", "--spec-type"});
    } else if (uses_mtp) {
        defaults.push_back({"--spec-type draft-mtp", "--spec-type"});
    }

    // An auto slot count also enables the unified KV buffer, which advertises the
    // full ctx_size to every slot instead of dividing it. Pinning the count keeps
    // ctx_size the context a request actually gets.
    defaults.push_back({"--parallel 1", "--parallel", {"-np"}});

    return append_runtime_arg_defaults(args, defaults);
}

// The resolved llamacpp_args string always contains at most one
// --sleep-idle-seconds occurrence (see resolve_llamacpp_runtime_args:
// append_runtime_arg_defaults only appends its computed default when a
// user-supplied one isn't already present), so the last-seen value is the
// effective one either way. Returns -1 (llama-server's own "disabled" value)
// if the flag is absent or its value doesn't parse.
static long parse_sleep_idle_seconds_arg(const std::string& args) {
    const auto tokens = parse_custom_args(args);
    const auto map = build_custom_args_map(tokens);
    auto it = map.find("--sleep-idle-seconds");
    if (it == map.end() || it->second.empty() || it->second.back().empty()) {
        return -1;
    }
    try {
        return std::stol(it->second.back().front());
    } catch (const std::exception&) {
        return -1;
    }
}

static std::string trim_version_prefix(const std::string& version) {
    if (!version.empty() && version[0] == 'v') {
        return version.substr(1);
    }
    return version;
}

static std::string trim_to_major_minor(const std::string& version) {
    // Trim to MAJOR.MINOR format (e.g., "7.12.0" -> "7.12")
    std::string trimmed = trim_version_prefix(version);
    size_t second_dot = trimmed.find('.', trimmed.find('.') + 1);
    if (second_dot != std::string::npos) {
        return trimmed.substr(0, second_dot);
    }
    return trimmed;
}

static std::string get_therock_version() {
    auto config = JsonUtils::load_from_file(utils::get_resource_path("resources/backend_versions.json"));
    if (!config.contains("therock") || !config["therock"].is_object() ||
        !config["therock"].contains("version") || !config["therock"]["version"].is_string()) {
        throw std::runtime_error("backend_versions.json is missing 'therock.version'");
    }
    return trim_to_major_minor(config["therock"]["version"].get<std::string>());
}

InstallParams LlamaCppServer::get_install_params(const std::string& backend, const std::string& version) {
    InstallParams params;

    const std::string resolved_backend = resolve_llamacpp_backend(backend);

    if (resolved_backend == "system") {
        return params; // Return empty params for system backend
    }

    if (resolved_backend == "rocm-stable") {
        params.repo = "lemonade-sdk/llama.cpp";
        std::string therock_ver = get_therock_version();
#ifdef _WIN32
        params.filename = "llama-" + version + "-bin-win-rocm-" + therock_ver + "-x64.zip";
#elif defined(__linux__)
        params.filename = "llama-" + version + "-bin-ubuntu-rocm-" + therock_ver + "-x64.tar.gz";
#else
        throw std::runtime_error("ROCm stable llamacpp is currently supported on Windows and Linux only");
#endif
    } else if (resolved_backend == "rocm-nightly") {
        params.repo = "lemonade-sdk/llamacpp-rocm";
        std::string target_arch =
            SystemInfo::rocm_asset_family(SystemInfo::get_rocm_arch());
        if (target_arch.empty()) {
            throw std::runtime_error(
                SystemInfo::get_unsupported_backend_error("llamacpp", "rocm-nightly")
            );
        }
#ifdef _WIN32
        params.filename = "llama-" + version + "-windows-rocm-" + target_arch + "-x64.zip";
#elif defined(__linux__)
        params.filename = "llama-" + version + "-ubuntu-rocm-" + target_arch + "-x64.zip";
#else
        throw std::runtime_error("ROCm nightly llamacpp only supported on Windows and Linux");
#endif
    } else if (resolved_backend == "cuda") {
        params.repo = "lemonade-sdk/llama.cpp";
        std::string target_arch = SystemInfo::get_cuda_arch();
        if (target_arch.empty()) {
            throw std::runtime_error(
                SystemInfo::get_unsupported_backend_error("llamacpp", "cuda")
            );
        }
        // lemonade-sdk/llama.cpp releases publish per-Compute-Capability binaries
        // and embed the build tag in the asset filename, e.g.
        // llama-b1011-ubuntu-cuda-sm_120-x64.tar.xz.
#ifdef _WIN32
        params.filename = "llama-" + version + "-windows-cuda-" + target_arch + "-x64.7z";
#elif defined(__linux__)
#if defined(__aarch64__)
        params.filename = "llama-" + version + "-ubuntu-cuda-" + target_arch + "-arm64.tar.xz";
#else
        params.filename = "llama-" + version + "-ubuntu-cuda-" + target_arch + "-x64.tar.xz";
#endif
#else
        throw std::runtime_error("CUDA llamacpp is currently supported on Windows and Linux only");
#endif
    } else if (resolved_backend == "metal") {
        params.repo = "ggml-org/llama.cpp";
#ifdef __APPLE__
        params.filename = "llama-" + version + "-bin-macos-arm64.tar.gz";
#else
        throw std::runtime_error("Metal llamacpp only supported on macOS");
#endif
    } else if (resolved_backend == "cpu") {
        params.repo = "ggml-org/llama.cpp";
#ifdef _WIN32
        params.filename = "llama-" + version + "-bin-win-cpu-x64.zip";
#elif defined(__linux__)
#if defined(__aarch64__)
        params.filename = "llama-" + version + "-bin-ubuntu-arm64.tar.gz";
#else
        params.filename = "llama-" + version + "-bin-ubuntu-x64.tar.gz";
#endif
#else
        throw std::runtime_error("CPU llamacpp not supported on this platform");
#endif
    } else {  // vulkan
        params.repo = "ggml-org/llama.cpp";
#ifdef _WIN32
        params.filename = "llama-" + version + "-bin-win-vulkan-x64.zip";
#elif defined(__linux__)
#if defined(__aarch64__)
        params.filename = "llama-" + version + "-bin-ubuntu-vulkan-arm64.tar.gz";
#else
        params.filename = "llama-" + version + "-bin-ubuntu-vulkan-x64.tar.gz";
#endif
#else
        throw std::runtime_error("Vulkan llamacpp only supported on Windows and Linux");
#endif
    }

    return params;
}

LlamaCppServer::LlamaCppServer(const std::string& log_level, ModelManager* model_manager, BackendManager* backend_manager)
    : WrappedServer("llama-server", log_level, model_manager, backend_manager) {
}

LlamaCppServer::~LlamaCppServer() {
    unload();
}

void LlamaCppServer::load(const std::string& model_name,
                         const ModelInfo& model_info,
                         const RecipeOptions& options,
                         bool do_not_upgrade) {
    LOG(INFO, "LlamaCpp") << "Loading model: " << model_name << std::endl;

    LOG(DEBUG, "LlamaCpp") << "Per-model settings: " << options.to_log_string() << std::endl;

    int ctx_size = options.get_option("ctx_size");

    std::string llamacpp_device = options.get_option("llamacpp_device");
    std::string llamacpp_backend_option = options.get_option("llamacpp_backend");
    std::string llamacpp_backend = resolve_llamacpp_backend(llamacpp_backend_option);
    std::string llamacpp_args = options.get_option("llamacpp_args");
    // A custom llamacpp_args value can override the --sleep-idle-seconds
    // Lemonade would otherwise compute from downsize_idle_timeout (see
    // resolve_llamacpp_runtime_args), including overriding it to -1
    // (llama-server's own "disabled" value). Parse the value that actually
    // ended up in the resolved args instead of just checking flag presence,
    // so both eligibility (sleep_idle_enabled_) and EvictionEngine's own
    // scheduling (effective_downsize_idle_timeout_sec()) track what's really
    // baked in, not the possibly-stale downsize_idle_timeout request.
    sleep_idle_seconds_effective_ = parse_sleep_idle_seconds_arg(llamacpp_args);
    sleep_idle_enabled_ = sleep_idle_seconds_effective_ >= 1;

    RuntimeConfig::validate_backend_choice("llamacpp", llamacpp_backend_option);

    LOG(INFO, "LlamaCpp") << "Using LlamaCpp Backend: " << llamacpp_backend << std::endl;

    bool use_gpu = (llamacpp_backend != "cpu");

    // Update device type based on the actual backend selected.
    device_type_ = use_gpu ? DEVICE_GPU : DEVICE_CPU;

    // Install llama-server if needed (use per-model backend)
    backend_manager_->install_backend(llamacpp::spec()->recipe, llamacpp_backend);

    // Use pre-resolved GGUF path. Skipped for hf_load models because llama-server
    // sources the weights itself via -hf; those models may not have local files.
    std::string gguf_path = model_info.resolved_path();
    if (gguf_path.empty() && !model_info.extra<bool>("hf_load", false)) {
        throw std::runtime_error("GGUF file not found for checkpoint: " + model_info.checkpoint());
    }

    if (!gguf_path.empty()) {
        LOG(DEBUG, "LlamaCpp") << "Using GGUF: " << gguf_path << std::endl;
    }

    // Get mmproj path for vision models and drafter path for mtp or other drafting strategies
    std::string mmproj_path = model_info.resolved_path("mmproj");
    std::string draft_path = model_info.resolved_path("draft");

    port_ = choose_port();

    std::string executable = BackendUtils::get_backend_binary_path(*llamacpp::spec(), llamacpp_backend);

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

    // hf_load delegates model+mmproj resolution to llama-server's -hf flag. This
    // is required for models like Qwen2.5-Omni where the manual -m + --mmproj
    // path rejects audio content parts in /v1/chat/completions — the -hf path
    // drives the dual-clip (vision+audio) context correctly.
    if (model_info.extra<bool>("hf_load", false)) {
        push_arg(args, reserved_flags, "-hf", model_info.checkpoint(),
                 std::vector<std::string>{"--hf-repo", "-mr", "--hf-file", "-mf"});
    } else {
        push_arg(args, reserved_flags, "-m", gguf_path, std::vector<std::string>{"--model"});
    }
    push_arg(args, reserved_flags, "--ctx-size", std::to_string(ctx_size), std::vector<std::string>{"-c"});

    if (!llamacpp_device.empty()) {
        BackendUtils::validate_device_backend_match(llamacpp_backend, llamacpp_device);
        push_arg(args, reserved_flags, "--device", llamacpp_device);
    }
    push_reserved(reserved_flags, "--device", std::vector<std::string>{"-dev"});

    push_arg(args, reserved_flags, "--port", std::to_string(port_));
    push_arg(args, reserved_flags, "--jinja", std::vector<std::string>{"--no-jinja"});
    push_arg(args, reserved_flags, "--metrics");

    LOG(DEBUG, "LlamaCpp") << "Using backend: " << llamacpp_backend << "\n"
            << "[LlamaCpp] Use GPU: " << (use_gpu ? "true" : "false") << std::endl;

    // Add mmproj file if present (for vision models). Skip when hf_load is set —
    // llama-server resolves the mmproj companion itself from the HF repo.
    if (!mmproj_path.empty() && !model_info.extra<bool>("hf_load", false)) {
        push_arg(args, reserved_flags, "--mmproj", mmproj_path);
        if (!use_gpu) {
            LOG(DEBUG, "LlamaCpp") << "Skipping mmproj argument since GPU mode is not enabled" << std::endl;
            push_arg(args, reserved_flags, "--no-mmproj-offload");
        }
    }
    push_reserved(reserved_flags, "--mmproj", std::vector<std::string>{"-mm", "-mmu", "--mmproj-url", "--no-mmproj", "--mmproj-auto", "--no-mmproj-auto"});

    const bool has_dflash_label =
        std::find(model_info.labels.begin(), model_info.labels.end(), "dflash") != model_info.labels.end();
    const bool is_dflash_draft =
        !draft_path.empty() && is_dflash_draft_checkpoint(model_info.checkpoint("draft"));
    const bool use_draft_checkpoint =
        !draft_path.empty() && (!is_dflash_draft || has_dflash_label);

    if (use_draft_checkpoint) {
        push_arg(args, reserved_flags, "--model-draft", draft_path);
    }
    push_reserved(reserved_flags, "--model-draft", std::vector<std::string>{"-md", "--spec-draft-model"});

    // Add embeddings support if the model supports it
    if (supports_embeddings) {
        LOG(INFO, "LlamaCpp") << "Model supports embeddings, adding --embeddings flag" << std::endl;
        push_arg(args, reserved_flags, "--embeddings");
    }
    push_reserved(reserved_flags, "--embeddings", std::vector<std::string>{"--embedding"});

    // Add reranking support if the model supports it
    if (supports_reranking) {
        LOG(INFO, "LlamaCpp") << "Model supports reranking, adding --reranking flag" << std::endl;
        push_arg(args, reserved_flags, "--reranking");
    }
    push_reserved(reserved_flags, "--reranking", std::vector<std::string>{"--rerank"});

    // Validate and append custom arguments
    if (!llamacpp_args.empty()) {
        std::string validation_error = validate_custom_args(llamacpp_args, reserved_flags);
        if (!validation_error.empty()) {
            throw std::invalid_argument(
                "Invalid custom llama-server arguments:\n" + validation_error
            );
        }

        LOG(DEBUG, "LlamaCpp") << "Adding custom arguments: " << llamacpp_args << std::endl;
        std::vector<std::string> custom_args_vec = parse_custom_args(llamacpp_args);
        args.insert(args.end(), custom_args_vec.begin(), custom_args_vec.end());
    }

    LOG(INFO, "LlamaCpp") << "Starting llama-server..." << std::endl;

    // For ROCm on Linux, set LD_LIBRARY_PATH to include the ROCm library directory
    std::vector<std::pair<std::string, std::string>> env_vars;
#ifndef _WIN32
    if (is_llamacpp_rocm_backend(llamacpp_backend)) {
        // Get the directory containing the executable (where ROCm .so files are)
        fs::path exe_dir = fs::path(executable).parent_path();
        std::string lib_path = exe_dir.string();

        if (llamacpp_backend == "rocm-stable") {
            std::string rocm_arch = SystemInfo::get_rocm_arch();
            if (!rocm_arch.empty()) {
                std::string therock_dirs = BackendUtils::join_runtime_dirs(
                    BackendUtils::get_therock_lib_paths(rocm_arch));
                if (!therock_dirs.empty()) {
                    lib_path = therock_dirs + ":" + lib_path;
                }
            }
        }

        // Preserve existing LD_LIBRARY_PATH if it exists
        const char* existing_ld_path = std::getenv("LD_LIBRARY_PATH");
        if (existing_ld_path && strlen(existing_ld_path) > 0) {
            lib_path = lib_path + ":" + std::string(existing_ld_path);
        }

        env_vars.push_back({"LD_LIBRARY_PATH", lib_path});
        LOG(DEBUG, "LlamaCpp") << "Setting LD_LIBRARY_PATH=" << lib_path << std::endl;
    } else if (is_llamacpp_cuda_backend(llamacpp_backend)) {
        // The llama.cpp-builds Linux tarballs ship the bundled CUDA runtime
        // (libcudart.so, libcublas.so, etc.) alongside llama-server, so add the
        // executable's directory to LD_LIBRARY_PATH like we do for ROCm.
        fs::path exe_dir = fs::path(executable).parent_path();
        std::string lib_path = exe_dir.string();

        const char* existing_ld_path = std::getenv("LD_LIBRARY_PATH");
        if (existing_ld_path && strlen(existing_ld_path) > 0) {
            lib_path = lib_path + ":" + std::string(existing_ld_path);
        }

        env_vars.push_back({"LD_LIBRARY_PATH", lib_path});
        LOG(DEBUG, "LlamaCpp") << "Setting LD_LIBRARY_PATH=" << lib_path << std::endl;
    }
#else
    // For ROCm on Windows with gfx1151, set OCL_SET_SVMSIZE
    // This is a patch to enable loading larger models
    if (is_llamacpp_rocm_backend(llamacpp_backend)) {
        std::string new_path;

        if (llamacpp_backend == "rocm-stable") {
            std::string rocm_arch = SystemInfo::get_rocm_arch();
            if (!rocm_arch.empty()) {
                // Prepend ALL TheRock runtime dirs (not just _rocm_sdk_core/bin) so
                // HIP + BLAS DLLs resolve; _rocm_sdk_core/bin alone → STATUS_DLL_NOT_FOUND.
                std::string therock_dirs = BackendUtils::join_runtime_dirs(
                    BackendUtils::get_therock_lib_paths(rocm_arch));
                if (!therock_dirs.empty()) {
                    new_path = therock_dirs;
                }
            }
        }

        if (!new_path.empty()) {
            const char* existing_path = std::getenv("PATH");
            if (existing_path && strlen(existing_path) > 0) {
                new_path += ";" + std::string(existing_path);
            }
            env_vars.push_back({"PATH", new_path});
        }

        // Windows DLL search order checks System32 BEFORE PATH, so a stale
        // System32 amdhip64_7.dll shadows the TheRock runtime on PATH. Copying
        // to the exe directory overrides both.
        if (llamacpp_backend == "rocm-stable") {
            std::string rocm_arch = SystemInfo::get_rocm_arch();
            if (!rocm_arch.empty()) {
                BackendUtils::stage_therock_hip_runtime(
                    rocm_arch, fs::path(executable).parent_path());
            }
        }

        std::string arch = lemon::SystemInfo::get_rocm_arch();
        if ((arch == "gfx1151") || (arch == "gfx1152")){
            env_vars.push_back({"OCL_SET_SVM_SIZE", "262144"});
            LOG(DEBUG, "LlamaCpp") << "Setting OCL_SET_SVM_SIZE=262144 for gfx1151/gfx1152 (enables loading larger models)" << std::endl;
        }
    } else if (is_llamacpp_cuda_backend(llamacpp_backend)) {
        // CUDA Windows builds bundle cudart64_*.dll, cublas64_*.dll, etc. next to
        // llama-server.exe. Prepend the executable directory to PATH so the loader
        // resolves them before any system-wide CUDA install.
        fs::path exe_dir = fs::absolute(fs::path(executable)).parent_path();
        std::string new_path = path_to_utf8(exe_dir);

        const char* existing_path = std::getenv("PATH");
        if (existing_path && strlen(existing_path) > 0) {
            new_path += ";" + std::string(existing_path);
        }
        env_vars.push_back({"PATH", new_path});
        LOG(DEBUG, "LlamaCpp") << "Prepending CUDA exe dir to PATH: " << path_to_utf8(exe_dir) << std::endl;
    }
#endif

    if (is_llamacpp_cuda_backend(llamacpp_backend)) {
        std::string existing_llama_device = utils::get_environment_variable_utf8("LLAMA_ARG_DEVICE");
        const bool has_llama_device_override = !existing_llama_device.empty();

        bool skip_visible_devices = false;
        if (!llamacpp_device.empty()) {
            LOG(INFO, "LlamaCpp")
                << "Using explicit llama.cpp CUDA device selection: " << llamacpp_device
                << std::endl;
            skip_visible_devices = true;
        } else if (has_llama_device_override) {
            LOG(INFO, "LlamaCpp")
                << "Respecting existing LLAMA_ARG_DEVICE=" << existing_llama_device
                << std::endl;
            skip_visible_devices = true;
        }
        BackendUtils::apply_cuda_env_vars(env_vars, "LlamaCpp", skip_visible_devices);
    }

#ifdef __APPLE__
    // Forward GGML_METAL_NO_RESIDENCY to llama-server if set in the parent
    // environment. Metal residency sets crash on paravirtualized GPUs (e.g.
    // GitHub Actions macOS runners with MTLGPUFamilyApple5).
    const char* no_residency = std::getenv("GGML_METAL_NO_RESIDENCY");
    if (no_residency) {
        env_vars.push_back({"GGML_METAL_NO_RESIDENCY", no_residency});
        LOG(DEBUG, "LlamaCpp") << "Forwarding GGML_METAL_NO_RESIDENCY=" << no_residency << std::endl;
    }

    // Ensure HOME is set in the child. llama.cpp b8884+ (libllama-common's
    // fs_get_cache_directory / hf_cache::migrate_old_cache_to_hf_cache)
    // calls getenv("HOME") during CLI arg parsing and passes the result
    // straight into std::string without a NULL check, segfaulting when
    // HOME is unset. LaunchDaemons installed at /Library/LaunchDaemons/
    // get a minimal env from launchd and do not inherit HOME, so llama-server
    // crashes before the model ever loads. Terminal/sudo spawns preserve
    // HOME and do not hit this.
    //
    // Upstream fix in flight: https://github.com/ggml-org/llama.cpp/pull/22263
    // Once that PR merges and lemonade's pinned llama.cpp version (in
    // src/cpp/resources/backend_versions.json) includes it, this HOME
    // fallback can be deleted.
    const char* home = std::getenv("HOME");
    if (!home || home[0] == '\0') {
        struct passwd* pw = getpwuid(getuid());
        std::string fallback_home = (pw && pw->pw_dir) ? pw->pw_dir : "/var/root";
        env_vars.push_back({"HOME", fallback_home});
        LOG(DEBUG, "LlamaCpp") << "Parent HOME unset; setting child HOME=" << fallback_home << std::endl;
    }
#endif

    // Start process (inherit output if debug logging enabled, filter health check spam)
    // Keep llama-server output visible at info log level.
    std::string process_executable = executable;
    std::string working_dir;
#ifdef _WIN32
    // Avoid inheriting a protected launcher cwd while keeping Linux/macOS behavior unchanged.
    fs::path executable_path = fs::absolute(fs::path(executable));
    process_executable = path_to_utf8(executable_path);
    working_dir = path_to_utf8(executable_path.parent_path());
#endif

    bool inherit_llama_output = (log_level_ == "info") || is_debug();
    set_process_handle(ProcessManager::start_process(
        process_executable, args, working_dir, inherit_llama_output, true, env_vars),
        process_executable, args);

    if (!wait_for_ready("/health")) {
        const ProcessHandle handle = consume_process_handle_for_cleanup();
        if (has_process_handle(handle)) {
            ProcessManager::stop_process(handle);
        }
        throw std::runtime_error("llama-server failed to start");
    }

    LOG(DEBUG, "LlamaCpp") << "Model loaded on port " << get_backend_port() << std::endl;
}

void LlamaCppServer::unload() {
    stop_backend_watchdog();
    LOG(INFO, "LlamaCpp") << "Unloading model..." << std::endl;

    const ProcessHandle handle = consume_process_handle_for_cleanup();
    if (has_process_handle(handle)) {
        ProcessManager::stop_process(handle);
    }
}

bool LlamaCppServer::downsize() {
    // Slot erase only clears bookkeeping (llama-server's SERVER_TASK_TYPE_SLOT_ERASE
    // calls prompt_clear(), never a backend free) — it destroys reusable KV state
    // without releasing any VRAM. Actual VRAM release is delegated to llama-server's
    // own --sleep-idle-seconds (passed at launch when auto_evict is enabled; see
    // resolve_llamacpp_runtime_args), which frees the whole model and transparently
    // reloads it on the next request. That reload is always a full re-prefill — the
    // host-RAM prompt cache is recreated empty on wake, so this is strictly better
    // than erase (VRAM is actually freed) but no faster to resume.
    if (!sleep_idle_enabled_) {
        // --sleep-idle-seconds isn't part of this instance's launch args (see
        // load()) -- auto_evict was off, or a pre-b7492 system backend hit the
        // version gate in resolve_llamacpp_runtime_args. There's no backend-side
        // sleep timer to verify against, so keep the old best-effort belief
        // rather than retrying forever every EvictionEngine tick.
        LOG(INFO, "LlamaCpp") << "Downsize delegated to llama-server's --sleep-idle-seconds, "
                                 "but this instance was launched without it; nothing to verify."
                              << std::endl;
        return true;
    }

    // Ground-truth check instead of assuming llama-server's independent
    // --sleep-idle-seconds timer fired just because Lemonade's own idle timer
    // did -- the two timers start from different "last activity" reference
    // points and can drift (see docs/dev/llamacpp-runtime-defaults.md). If not
    // asleep yet, return false; EvictionEngine's finish_downsize(false) reverts
    // to READY and the model stays an idle candidate, so this is retried on the
    // next EvictionEngine tick for free.
    const json props = forward_get_request("/props");
    const bool is_sleeping = props.value("is_sleeping", false);
    LOG(INFO, "LlamaCpp") << "Downsize check via /props: is_sleeping=" << is_sleeping
                          << std::endl;
    return is_sleeping;
}

bool LlamaCppServer::downsize_effective_for_this_instance(bool /*auto_evict_config*/) const {
    return sleep_idle_enabled_;
}

long LlamaCppServer::effective_downsize_idle_timeout_sec() const {
    // Only meaningful once downsize_effective_for_this_instance() is true, at
    // which point sleep_idle_seconds_effective_ is guaranteed >= 1 -- return
    // -1 (no override) otherwise so a caller can't mistake "disabled" for a
    // real 0-or-negative timeout.
    return sleep_idle_enabled_ ? sleep_idle_seconds_effective_ : -1;
}

json LlamaCppServer::normalize_response_model(json response, const json& request) const {
    if (response.is_object() && response.contains("model")) {
        response["model"] = request.value("model", get_model_name());
    }
    return response;
}

json LlamaCppServer::chat_completion(const json& request) {
    return normalize_response_model(
        forward_request("/v1/chat/completions",
                        llamacpp::sanitize_tool_schema_limits(
                            JsonUtils::with_legacy_max_tokens_alias(request))),
        request);
}

json LlamaCppServer::completion(const json& request) {
    return normalize_response_model(
        forward_request("/v1/completions",
                        JsonUtils::with_legacy_max_tokens_alias(request)),
        request);
}

json LlamaCppServer::embeddings(const json& request) {
    return forward_request("/v1/embeddings", request);
}

json LlamaCppServer::reranking(const json& request) {
    return forward_request("/v1/rerank", request);
}

json LlamaCppServer::get_slots() {
    return forward_get_request("/slots");
}

json LlamaCppServer::slots_action(int slot_id, const std::string& action, const json& request_body) {
    return forward_request("/slots/" + std::to_string(slot_id) + "?action=" + action, request_body);
}

json LlamaCppServer::tokenize(const json& request_body) {
    return forward_request("/tokenize", request_body);
}

json LlamaCppServer::responses(const json& request) {
    return normalize_response_model(
        forward_request("/v1/responses",
                        llamacpp::sanitize_tool_schema_limits(request)),
        request);
}

void LlamaCppServer::forward_streaming_request(const std::string& endpoint,
                                               const std::string& request_body,
                                               httplib::DataSink& sink,
                                               bool sse,
                                               long timeout_seconds,
                                               TelemetryCallback telemetry_callback) {
    std::string body = request_body;
    if (endpoint == "/v1/chat/completions" || endpoint == "/v1/responses") {
        json request = json::parse(request_body, nullptr, false);
        if (!request.is_discarded()) {
            if (endpoint == "/v1/chat/completions") {
                JsonUtils::add_legacy_max_tokens_alias(request);
            }
            body = llamacpp::sanitize_tool_schema_limits(std::move(request)).dump();
        }
    }

    WrappedServer::forward_streaming_request(
        endpoint, body, sink, sse, timeout_seconds, telemetry_callback);
}

} // namespace backends
} // namespace lemon

namespace lemon {
namespace backends {
namespace llamacpp {

std::unique_ptr<WrappedServer> create(const BackendContext& ctx) {
    return make_server<LlamaCppServer>(ctx);
}

namespace {
std::string system_llamacpp_version() {
    std::string output;
    #ifdef _WIN32
    std::string command = "llama-server --version 2>NUL";
    int rc = lemon::utils::ProcessManager::run_command(command, output);
    #else
    FILE* pipe = popen("llama-server --version 2>/dev/null", "r");
    if (!pipe) {
        return "unknown";
    }

    char buffer[256];
    if (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output = buffer;
    }

    pclose(pipe);
    #endif

    // Parse version from output like "version: 3432 (e2b2a632)" or "llama.cpp version b3432"
    if (!output.empty()) {
        // Try to find a version number
        std::regex version_regex(R"(version:\s*(\d+)|version\s+b?(\d+))");
        std::smatch match;
        if (std::regex_search(output, match, version_regex)) {
            for (size_t i = 1; i < match.size(); ++i) {
                if (match[i].matched) {
                    return "b" + match[i].str();
                }
            }
        }
        return "detected";
    }

    return "unknown";
}

// Numeric build tag embedded in a "bNNNN" version string (see
// system_llamacpp_version() above), or -1 if unparseable ("detected"/"unknown").
long parse_llamacpp_build_number(const std::string& version) {
    if (version.size() < 2 || version[0] != 'b') return -1;
    try {
        return std::stol(version.substr(1));
    } catch (const std::exception&) {
        return -1;
    }
}

// llama.cpp added --sleep-idle-seconds in build b7492 ("server: add auto-sleep
// after N seconds of idle", ddcb75dd8ac42dc23eb84f13bb17670fe9f2d49b). Lemonade's
// own pinned/managed llama-server binaries are all well above this build, so
// only a PATH-installed "system" backend can predate it and silently ignore
// the flag.
//
// resolve_runtime_options() runs on every request that resolves effective
// options (not just at model-load time — see server.cpp's
// resolve_context_length() and the model-detail handler), so the version probe
// is memoized here rather than re-spawning `llama-server --version` per call.
constexpr long kMinSleepIdleSecondsBuild = 7492;

bool system_llamacpp_supports_sleep_idle_seconds() {
    static const bool supported =
        parse_llamacpp_build_number(system_llamacpp_version()) >= kMinSleepIdleSecondsBuild;
    return supported;
}

bool is_ggml_hip_plugin_available() {
#ifdef __linux__
    // Allow distros/packagers that install outside the FHS paths below
    // (e.g. NixOS, custom prefixes) to point directly at libggml-hip.so.
    if (detail::ggml_hip_env_override_available()) {
        return true;
    }

    // A self-built llama.cpp resolved from PATH ships libggml-hip.so next to
    // the binary (build tree) or in a sibling lib/ directory (installed tree).
    // find_executable_in_path() returns the bare name on POSIX, so walk PATH
    // here to recover the directory the binary actually lives in.
    if (const char* path_env = std::getenv("PATH"); path_env && *path_env) {
        if (detail::ggml_hip_plugin_near_llama_server(path_env)) {
            return true;
        }
    }

    // On Linux x86_64, check common system library paths for the HIP plugin
    std::vector<std::string> possible_paths = {
        // Debian/Ubuntu multiarch path (most common)
        "/usr/lib/x86_64-linux-gnu/ggml/backends0/libggml-hip.so",
        // Arch package paths
        "/usr/lib/libggml-hip.so",
        "/usr/lib/ggml/libggml-hip.so",
        "/usr/lib64/ggml/libggml-hip.so",
        // Standard Linux paths
        "/usr/lib/ggml/backends0/libggml-hip.so",
        "/usr/lib64/ggml/backends0/libggml-hip.so"
    };

    for (const auto& path : possible_paths) {
        if (fs::exists(path)) {
            return true;
        }
    }
#endif

    return false;
}


// llamacpp model-management behavior: GGUF metadata + capability labels.
class LlamaCppOps : public BackendOps {
public:
    void resolve_runtime_options(const ModelInfo& info, RecipeOptions& options) const override {
        const json merge_args_value = options.get_option("merge_args");
        const bool merge_args =
            merge_args_value.is_boolean() ? merge_args_value.get<bool>() : true;
        const json custom_args_value = options.get_option("llamacpp_args");
        const std::string custom_args =
            custom_args_value.is_string() ? custom_args_value.get<std::string>() : "";

        const json auto_evict_value = options.get_option("auto_evict");
        const bool auto_evict = auto_evict_value.is_boolean()
                                     ? auto_evict_value.get<bool>()
                                     : RuntimeConfig::global()->auto_evict();
        long sleep_idle_seconds = -1;
        if (auto_evict) {
            const json downsize_timeout_value = options.get_option("downsize_idle_timeout");
            // is_number_integer() is false for a JSON value that arrived as a float
            // even when it holds a whole number (e.g. 3.0), which would silently
            // fall back to the default below. is_number() plus truncation covers
            // both representations.
            sleep_idle_seconds = downsize_timeout_value.is_number()
                                      ? static_cast<long>(downsize_timeout_value.get<double>())
                                      : kDefaultDownsizeIdleTimeoutSec;

            // A PATH-installed "system" llama-server predating --sleep-idle-seconds
            // (build b7492) would silently ignore the flag, so don't pass it — the
            // model would just never downsize instead of failing to load.
            const std::string backend =
                resolve_llamacpp_backend(options.get_option("llamacpp_backend"));
            if (backend == "system" && !system_llamacpp_supports_sleep_idle_seconds()) {
                sleep_idle_seconds = -1;
            }
        }

        options.set_option(
            "llamacpp_args",
            resolve_llamacpp_runtime_args(info, custom_args, merge_args, sleep_idle_seconds));
    }

    void populate_metadata(ModelInfo& info, const BackendOpsContext&) const override {
        const std::string gguf_path = info.resolved_path();
        if (gguf_path.size() < 5) {
            return;
        }
        std::string ext = gguf_path.substr(gguf_path.size() - 5);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext != ".gguf") {
            return;
        }
        std::error_code ec;
        if (!std::filesystem::exists(lemon::utils::path_from_utf8(gguf_path), ec)) {
            return;
        }
        GgufMetadata meta;
        if (!read_gguf_metadata(meta, gguf_path)) {
            return;
        }
        info.max_context_window = meta.context_length;
        info.gguf = std::move(meta);
        // GGUF vision/tool metadata are LLM capabilities. Don't apply them to
        // embedding/reranking models, or labels like tool-calling would
        // reclassify the model away from its endpoint type.
        if (info.type == ModelType::LLM) {
            apply_gguf_capability_labels(info.labels, info.gguf.caps);
        }
    }

    std::string resolve_checkpoint_path(const ModelInfo& info,
                                        const CheckpointResolveContext& ctx) const override {
        // The main checkpoint is a GGUF file (with sharding/variant resolution);
        // auxiliary checkpoints (mmproj, …) use the shared default.
        if (ctx.type == "main") {
            return resolve_gguf_path(ctx.model_cache_path, ctx.variant);
        }
        return BackendOps::resolve_checkpoint_path(info, ctx);
    }

    std::string find_imported_checkpoint(const std::string& import_dir) const override {
        // The primary artifact is the (non-mmproj) GGUF file.
        return resolve_gguf_path(import_dir, "");
    }

    std::string validate_registration_checkpoint(const std::string& checkpoint) const override {
        // A GGUF checkpoint must name its quant via CHECKPOINT:VARIANT.
        std::string lower = checkpoint;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (lower.find("gguf") != std::string::npos &&
            checkpoint.find(':') == std::string::npos) {
            return "You are required to provide a 'variant' in the checkpoint field when "
                   "registering a GGUF model. The variant is provided as CHECKPOINT:VARIANT. "
                   "For example: Qwen/Qwen2.5-Coder-3B-Instruct-GGUF:Q4_0 or "
                   "Qwen/Qwen2.5-Coder-3B-Instruct-GGUF:qwen2.5-coder-3b-instruct-q4_0.gguf";
        }
        return "";
    }

    std::string validate_checkpoint_file(const std::string& resolved_path) const override {
        // A .gguf file in the cache must start with the GGUF magic, else it's a
        // truncated/corrupt download and the model is not really present.
        std::error_code ec;
        std::filesystem::path p = lemon::utils::path_from_utf8(resolved_path);
        if (std::filesystem::is_directory(p, ec)) {
            return "";
        }
        std::string ext = resolved_path.size() >= 5 ? resolved_path.substr(resolved_path.size() - 5) : "";
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext != ".gguf") {
            return "";
        }
        std::ifstream in(p, std::ios::binary);
        char magic[4] = {};
        in.read(magic, sizeof(magic));
        bool ok = in.gcount() == static_cast<std::streamsize>(sizeof(magic)) &&
                  magic[0] == 'G' && magic[1] == 'G' && magic[2] == 'U' && magic[3] == 'F';
        return ok ? "" : "Invalid GGUF cache file";
    }

    std::string resolve_version(const std::string& backend,
                                const std::string& file_version) const override {
        // The PATH-installed "system" llama-server has no version.txt; query it.
        if (backend == "system") {
            return system_llamacpp_version();
        }
        return file_version;
    }

    InstallCheck check_install(const std::string& backend, bool binary_found) const override {
        // The system llama-server also needs the ggml HIP plugin for ROCm GPU
        // acceleration when an AMD GPU (KFD) is present.
        if (binary_found && backend == "system") {
#ifdef __linux__
            if (std::filesystem::exists("/sys/class/kfd") && !is_ggml_hip_plugin_available()) {
                return {false, "HIP plugin libggml-hip.so not installed"};
            }
#endif
        }
        return {binary_found, ""};
    }
};
}  // namespace

const BackendSpec* spec() { return make_spec<LlamaCppServer>(descriptor); }
const BackendOps* ops() { return single_ops<LlamaCppOps>(); }
}  // namespace llamacpp
}  // namespace backends
}  // namespace lemon
