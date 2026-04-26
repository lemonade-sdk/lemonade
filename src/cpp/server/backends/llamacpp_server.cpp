#include "lemon/backends/llamacpp_server.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backend_manager.h"
#include "lemon/runtime_config.h"
#include "lemon/utils/custom_args.h"
#include "lemon/utils/process_manager.h"
#include "lemon/utils/json_utils.h"
#include "lemon/utils/http_client.h"
#include "lemon/utils/path_utils.h"
#include "lemon/error_types.h"
#include "lemon/system_info.h"
#include "lemon/memory_manager.h"
#include <iostream>
#include <filesystem>
#include <lemon/utils/aixlog.hpp>
#include <cstdlib>
#include <cstdint>
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

// Embedding model batch configuration set to 8192 as default
static const int EMBEDDING_CTX_SIZE = 8192;
static const int EMBEDDING_BATCH_SIZE = 8192;
static const int EMBEDDING_UBATCH_SIZE = 8192;
static const char* ROCM_STABLE_RUNTIME_DIR = "rocm-stable-runtime";


static int read_loaded_context_size_from_props(const std::string& base_url) {
    try {
        auto response = utils::HttpClient::get(base_url + "/props");
        if (response.status_code != 200 || response.body.empty()) {
            return 0;
        }

        auto props = nlohmann::json::parse(response.body);
        if (props.contains("default_generation_settings") &&
            props["default_generation_settings"].is_object() &&
            props["default_generation_settings"].contains("n_ctx")) {
            return props["default_generation_settings"]["n_ctx"].get<int>();
        }
    } catch (const std::exception& e) {
        LOG(DEBUG, "LlamaCpp") << "Could not query /props for actual context size: "
                               << e.what() << std::endl;
    }
    return 0;
}

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

static int round_context_down_to_step(int ctx) {
    if (ctx <= MemoryManager::kProbeContext) {
        return MemoryManager::kProbeContext;
    }
    constexpr int kStep = 1024;
    return std::max(MemoryManager::kProbeContext, (ctx / kStep) * kStep);
}

static int expected_context_without_resource_restriction(const ModelMemoryEstimate& estimate,
                                                         int fallback_context) {
    int expected = estimate.target_context > 0 ? estimate.target_context : fallback_context;
    if (estimate.model_max_context > 0) {
        expected = std::min(expected, estimate.model_max_context);
    }
    if (expected > 0) {
        expected = std::max(expected, MemoryManager::kProbeContext);
    }
    return expected;
}

static bool should_warn_restricted_context(const ModelMemoryEstimate& estimate,
                                           int loaded_context) {
    if (loaded_context <= 0) {
        return false;
    }

    const int expected = expected_context_without_resource_restriction(estimate, loaded_context);
    if (expected <= 0 || loaded_context >= expected) {
        // The user or the model metadata intentionally requested/allowed this
        // size. Do not present that as a resource-shortage warning.
        return false;
    }

    return loaded_context <= MemoryManager::kProbeContext ||
           loaded_context < MemoryManager::kRestrictedContextWarningThreshold;
}

static void apply_restricted_context_warning(ModelMemoryEstimate& estimate, int loaded_context) {
    if (!should_warn_restricted_context(estimate, loaded_context)) {
        return;
    }

    const int expected = expected_context_without_resource_restriction(estimate, loaded_context);
    estimate.restricted_context_warning = true;
    estimate.warning = "Context size severely restricted due to resource limitations. Model loaded with "
        + std::to_string(loaded_context) + " context (expected "
        + std::to_string(expected) + ").";
    LOG(WARNING, "LlamaCpp") << estimate.warning << std::endl;
}

static int calculate_strict_ram_context_cap(const ModelMemoryEstimate& estimate,
                                            const SystemMemoryProbe& probe,
                                            int context_target) {
    int target = context_target > 0 ? context_target : MemoryManager::kDefaultContextTarget;
    if (estimate.model_max_context > 0) {
        target = std::min(target, estimate.model_max_context);
    }
    target = std::max(target, MemoryManager::kProbeContext);

    // Without an explicit Lemonade RAM limit there is nothing to cap here:
    // llama.cpp's --fit should use the real available system/device memory.
    if (probe.ram_limit_bytes == 0) {
        return target;
    }

    const uint64_t host_base_required = estimate.host_base_required_bytes > 0
        ? estimate.host_base_required_bytes
        : estimate.base_required_bytes;
    if (host_base_required >= probe.effective_available_bytes) {
        return MemoryManager::kProbeContext;
    }

    uint64_t budget = probe.effective_available_bytes - host_base_required;

    // Keep a small margin for process/runtime allocations that are not part of
    // the GGUF KV estimate. This is intentionally modest: too much conservatism
    // makes tight RAM limits unusable, and llama.cpp will still fit against real
    // device/system memory during startup.
    constexpr uint64_t kSafetyMarginBytes = 128ULL * 1024ULL * 1024ULL;
    if (budget > kSafetyMarginBytes) {
        budget -= kSafetyMarginBytes;
    } else {
        budget = 0;
    }

    uint64_t bytes_per_token = estimate.separate_device_memory
        ? estimate.host_kv_cache_bytes_per_token
        : (estimate.host_kv_cache_bytes_per_token > 0
            ? estimate.host_kv_cache_bytes_per_token
            : estimate.kv_cache_bytes_per_token);
    if (bytes_per_token == 0) {
        return target;
    }
    uint64_t estimated_ctx = budget / bytes_per_token;
    if (estimated_ctx > static_cast<uint64_t>(target)) {
        estimated_ctx = static_cast<uint64_t>(target);
    }
    if (estimated_ctx < static_cast<uint64_t>(MemoryManager::kProbeContext)) {
        estimated_ctx = static_cast<uint64_t>(MemoryManager::kProbeContext);
    }
    if (estimated_ctx > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
        estimated_ctx = static_cast<uint64_t>(std::numeric_limits<int>::max());
    }

    return round_context_down_to_step(static_cast<int>(estimated_ctx));
}

static std::string resolve_llamacpp_backend(const std::string& backend) {
    if (backend == "rocm") {
        // Map "rocm" to the appropriate channel based on config
        std::string channel = "preview";  // default to preview for now
        if (auto* cfg = RuntimeConfig::global()) {
            channel = cfg->rocm_channel();
        }
        return "rocm-" + channel;
    }
    return backend;
}

static bool is_llamacpp_rocm_backend(const std::string& backend) {
    return backend == "rocm-stable" || backend == "rocm-preview" || backend == "rocm-nightly";
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

    if (resolved_backend == "rocm-preview") {
        params.repo = "lemonade-sdk/llama.cpp";
        std::string therock_ver = get_therock_version();
#ifdef _WIN32
        params.filename = "llama-" + version + "-bin-win-rocm-" + therock_ver + "-x64.zip";
#elif defined(__linux__)
        params.filename = "llama-" + version + "-bin-ubuntu-rocm-" + therock_ver + "-x64.tar.gz";
#else
        throw std::runtime_error("ROCm preview llamacpp is currently supported on Windows and Linux only");
#endif
    } else if (resolved_backend == "rocm-nightly") {
        params.repo = "lemonade-sdk/llamacpp-rocm";
        std::string target_arch = SystemInfo::get_rocm_arch();
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
    } else if (resolved_backend == "rocm-stable") {
        params.repo = "ggml-org/llama.cpp";
#ifdef _WIN32
        params.filename = "llama-" + version + "-bin-win-hip-radeon-x64.zip";
#elif defined(__linux__)
        params.filename = "llama-" + version + "-bin-ubuntu-rocm-7.2-x64.tar.gz";
#else
        throw std::runtime_error("ROCm stable llamacpp is currently supported on Windows and Linux only");
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
        params.filename = "llama-" + version + "-bin-ubuntu-x64.tar.gz";
#else
        throw std::runtime_error("CPU llamacpp not supported on this platform");
#endif
    } else {  // vulkan
        params.repo = "ggml-org/llama.cpp";
#ifdef _WIN32
        params.filename = "llama-" + version + "-bin-win-vulkan-x64.zip";
#elif defined(__linux__)
        params.filename = "llama-" + version + "-bin-ubuntu-vulkan-x64.tar.gz";
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

    // Llamacpp Backend logging
    LOG(DEBUG, "LlamaCpp") << "Per-model settings: " << options.to_log_string() << std::endl;

    int ctx_size = options.get_option("ctx_size");
    std::string llamacpp_backend_option = options.get_option("llamacpp_backend");
    std::string llamacpp_backend = resolve_llamacpp_backend(llamacpp_backend_option);
    std::string llamacpp_args = options.get_option("llamacpp_args");

    RuntimeConfig::validate_backend_choice("llamacpp", llamacpp_backend_option);

    LOG(INFO, "LlamaCpp") << "Using LlamaCpp Backend: " << llamacpp_backend << std::endl;

    bool use_gpu = (llamacpp_backend != "cpu");

    // Update device type based on the actual backend selected.
    // get_device_type_from_recipe() defaults llamacpp to GPU, but the cpu backend runs on CPU.
    device_type_ = use_gpu ? DEVICE_GPU : DEVICE_CPU;

    // Install llama-server if needed (use per-model backend)
    backend_manager_->install_backend(SPEC.recipe, llamacpp_backend);

    // Use pre-resolved GGUF path
    std::string gguf_path = model_info.resolved_path();
    if (gguf_path.empty()) {
        throw std::runtime_error("GGUF file not found for checkpoint: " + model_info.checkpoint());
    }

    LOG(DEBUG, "LlamaCpp") << "Using GGUF: " << gguf_path << std::endl;

    // Get mmproj path for vision models
    std::string mmproj_path = model_info.resolved_path("mmproj");

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

    MemoryBackendClass memory_backend = use_gpu ? MemoryBackendClass::GPU : MemoryBackendClass::CPU;
    long long runtime_ram_limit = -1;
    if (auto* cfg = RuntimeConfig::global()) {
        runtime_ram_limit = cfg->ram_limit();
    }

    ModelMemoryEstimate memory_estimate;
    SystemMemoryProbe before_probe;
    bool dynamic_context = false;
    bool dynamic_fit_via_llamacpp = false;
    std::string gguf_architecture;

    if (!supports_embeddings && !supports_reranking) {
        if (auto* cfg = RuntimeConfig::global()) {
            int context_target = ctx_size;
            if (context_target > 0) {
                dynamic_context = true;
                before_probe = MemoryManager::probe_system_memory(runtime_ram_limit);
                memory_estimate = MemoryManager::estimate_llamacpp_memory(
                    model_info, gguf_path, memory_backend, context_target, runtime_ram_limit);

                if (memory_estimate.hard_error) {
                    throw MemoryPreflightException(memory_estimate.warning);
                }

                if (memory_estimate.model_max_context > 0) {
                    ctx_size = std::min(ctx_size, memory_estimate.model_max_context);
                }
                ctx_size = std::max(ctx_size, MemoryManager::kProbeContext);

                const bool strict_ram_limit = runtime_ram_limit >= 0;
                int pre_cap_ctx_size = ctx_size;
                if (strict_ram_limit) {
                    pre_cap_ctx_size = calculate_strict_ram_context_cap(memory_estimate, before_probe, ctx_size);
                    if (pre_cap_ctx_size < ctx_size) {
                        LOG(WARNING, "LlamaCpp")
                            << "RAM limit reduced context target before llama.cpp startup from "
                            << ctx_size << " to " << pre_cap_ctx_size
                            << ". This avoids a slow load/kill/reload loop; llama.cpp cannot shrink "
                            << "an already-created context in the external llama-server process."
                            << std::endl;
                        ctx_size = pre_cap_ctx_size;
                    }
                }

                gguf_architecture = MemoryManager::get_llamacpp_architecture(gguf_path);
                dynamic_fit_via_llamacpp = memory_estimate.model_max_context > 0 && !gguf_architecture.empty();

                LOG(INFO, "LlamaCpp")
                    << "Dynamic context sizing enabled: context_target=" << context_target
                    << ", effective_target=" << ctx_size
                    << ", model_max_ctx=" << memory_estimate.model_max_context
                    << ", available/allowed=" << MemoryManager::format_bytes(before_probe.effective_available_bytes)
                    << ", base_required=" << MemoryManager::format_bytes(memory_estimate.base_required_bytes)
                    << ", host_base_required=" << MemoryManager::format_bytes(memory_estimate.host_base_required_bytes)
                    << ", device_base_required=" << MemoryManager::format_bytes(memory_estimate.device_base_required_bytes)
                    << ", memory_domain=" << memory_estimate.memory_domain
                    << ", kv_per_token=" << memory_estimate.kv_cache_bytes_per_token
                    << " bytes"
                    << ", host_kv_per_token=" << memory_estimate.host_kv_cache_bytes_per_token
                    << " bytes"
                    << ", llama_fit=" << (dynamic_fit_via_llamacpp ? "enabled" : "unavailable")
                    << ", ram_limit_mode="
                    << (strict_ram_limit ? "strict-prelaunch-context-cap" : "not-set")
                    << std::endl;

                if (strict_ram_limit) {
                    LOG(INFO, "LlamaCpp")
                        << "RAM limit is handled before startup: Lemonade caps the requested context, "
                        << "disables llama-server prompt cache, then lets llama.cpp fit within that cap. "
                        << "The external llama-server process cannot shrink context after it is loaded."
                        << std::endl;
                }
            }
        }
    }

    // For ROCm on Linux, set LD_LIBRARY_PATH to include the ROCm library directory
    std::vector<std::pair<std::string, std::string>> env_vars;
#ifndef _WIN32
    if (is_llamacpp_rocm_backend(llamacpp_backend)) {
        // Get the directory containing the executable (where ROCm .so files are)
        fs::path exe_dir = fs::path(executable).parent_path();
        std::string lib_path = exe_dir.string();

        if (llamacpp_backend == "rocm-stable") {
            std::string runtime_dir = BackendUtils::get_install_directory(ROCM_STABLE_RUNTIME_DIR, "");
            if (fs::exists(runtime_dir)) {
                lib_path = runtime_dir + ":" + lib_path;
            }
        } else if (llamacpp_backend == "rocm-preview") {
            std::string rocm_arch = SystemInfo::get_rocm_arch();
            if (!rocm_arch.empty()) {
                std::string therock_lib = BackendUtils::get_therock_lib_path(rocm_arch);
                if (!therock_lib.empty()) {
                    lib_path = therock_lib + ":" + lib_path;
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
    }
#else
    // For ROCm on Windows with gfx1151, set OCL_SET_SVMSIZE
    // This is a patch to enable loading larger models
    if (is_llamacpp_rocm_backend(llamacpp_backend)) {
        std::string new_path;

        if (llamacpp_backend == "rocm-stable") {
            std::string runtime_dir = BackendUtils::get_install_directory(ROCM_STABLE_RUNTIME_DIR, "");
            if (fs::exists(runtime_dir)) {
                new_path = runtime_dir;
            }
        } else if (llamacpp_backend == "rocm-preview") {
            std::string rocm_arch = SystemInfo::get_rocm_arch();
            if (!rocm_arch.empty()) {
                std::string therock_bin = BackendUtils::get_therock_lib_path(rocm_arch);
                if (!therock_bin.empty()) {
                    new_path = therock_bin;
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

        std::string arch = lemon::SystemInfo::get_rocm_arch();
        if (arch == "gfx1151") {
            env_vars.push_back({"OCL_SET_SVM_SIZE", "262144"});
            LOG(DEBUG, "LlamaCpp") << "Setting OCL_SET_SVM_SIZE=262144 for gfx1151 (enables loading larger models)" << std::endl;
        }
    }
#endif

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

    auto build_args = [&](int selected_ctx_size) {
        std::vector<std::string> args;
        std::set<std::string> reserved_flags;

        push_arg(args, reserved_flags, "-m", gguf_path, std::vector<std::string>{"--model"});

        int llama_ctx_arg = selected_ctx_size;
        if (dynamic_context && dynamic_fit_via_llamacpp) {
            // Let llama.cpp calculate the largest safe context during context
            // creation. The context target is enforced by lowering the model
            // metadata context_length when the model advertises a larger one.
            llama_ctx_arg = 0;
        }
        push_arg(args, reserved_flags, "--ctx-size", std::to_string(llama_ctx_arg), std::vector<std::string>{"-c"});

        if (dynamic_context && dynamic_fit_via_llamacpp) {
            push_arg(args, reserved_flags, "--fit", "on", std::vector<std::string>{"-fit"});
            push_arg(args, reserved_flags, "--fit-ctx", std::to_string(MemoryManager::kProbeContext), std::vector<std::string>{"-fitc"});
            if (memory_estimate.model_max_context > selected_ctx_size && !gguf_architecture.empty()) {
                push_arg(args, reserved_flags, "--override-kv",
                         gguf_architecture + ".context_length=int:" + std::to_string(selected_ctx_size));
            }
        }

        push_arg(args, reserved_flags, "--port", std::to_string(port_));
        push_arg(args, reserved_flags, "--jinja", std::vector<std::string>{"--no-jinja"});

        LOG(DEBUG, "LlamaCpp") << "Using backend: " << llamacpp_backend << "\n"
                << "[LlamaCpp] Use GPU: " << (use_gpu ? "true" : "false") << std::endl;

        // Add mmproj file if present (for vision models)
        if (!mmproj_path.empty()) {
            push_arg(args, reserved_flags, "--mmproj", mmproj_path);
            if (!use_gpu) {
                LOG(DEBUG, "LlamaCpp") << "Skipping mmproj argument since GPU mode is not enabled" << std::endl;
                push_arg(args, reserved_flags, "--no-mmproj-offload");
            }
        }
        push_reserved(reserved_flags, "--mmproj", std::vector<std::string>{"-mm", "-mmu", "--mmproj-url", "--no-mmproj", "--mmproj-auto", "--no-mmproj-auto", "--mmproj-offload", "--no-mmproj-offload"});

        // Enable context shift for vulkan/rocm (not supported on Metal)
        if (llamacpp_backend == "vulkan" || is_llamacpp_rocm_backend(llamacpp_backend)) {
            push_overridable_arg(args, llamacpp_args, "--context-shift");
            push_overridable_arg(args, llamacpp_args, "--keep", "16");
        } else {
            // For Metal, just use keep without context-shift
            push_overridable_arg(args, llamacpp_args, "--keep", "16");
        }

        // Use legacy reasoning formatting
        push_overridable_arg(args, llamacpp_args, "--reasoning-format", "auto");

        // llama-server has an idle prompt cache that can reserve/use a large
        // amount of host RAM. Keep the upstream default for normal runs, but
        // disable it when the user explicitly configured a Lemonade RAM limit.
        if (runtime_ram_limit >= 0) {
            push_overridable_arg(args, llamacpp_args, "--cache-ram", "0");
        }

        // Disable llamacpp webui by default
        push_overridable_arg(args, llamacpp_args, "--no-webui");

        // Disable mmap on iGPU
        if (SystemInfo::get_has_igpu()) {
            push_overridable_arg(args, llamacpp_args, "--no-mmap");
        }

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

        // Configure GPU layers
        std::string gpu_layers = use_gpu ? "99" : "0";  // 99 for GPU, 0 for CPU-only
        LOG(DEBUG, "LlamaCpp") << "ngl set to " << gpu_layers << std::endl;
        push_arg(args, reserved_flags, "-ngl", gpu_layers, std::vector<std::string>{"--gpu-layers", "--n-gpu-layers"});

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

        return args;
    };

    auto start_with_ctx = [&](int selected_ctx_size) -> int {

        std::vector<std::string> args = build_args(selected_ctx_size);
        LOG(INFO, "LlamaCpp") << "Starting llama-server with ctx_size="
                              << selected_ctx_size
                              << (dynamic_context && dynamic_fit_via_llamacpp ? " (llama.cpp fit enabled)" : "")
                              << "..." << std::endl;

        // Start process (inherit output if debug logging enabled, filter health check spam)
        // Keep llama-server output visible at info log level.
        bool inherit_llama_output = (log_level_ == "info") || is_debug();
        process_handle_ = ProcessManager::start_process(executable, args, "", inherit_llama_output, true, env_vars);

        // Wait for server to be ready
        if (!wait_for_ready("/health")) {
            ProcessManager::stop_process(process_handle_);
            process_handle_ = {nullptr, 0};  // Reset to prevent double-stop on destructor
            throw std::runtime_error(
                "llama-server failed to start. The base model plus minimum context probably does not fit in the available/allowed memory.");
        }

        int actual_ctx_size = read_loaded_context_size_from_props(get_base_url());
        if (actual_ctx_size <= 0) {
            actual_ctx_size = selected_ctx_size;
        }


        return actual_ctx_size;
    };

    if (dynamic_context) {
        port_ = choose_port();
        int actual_ctx_size = start_with_ctx(ctx_size);

        memory_estimate.final_context = actual_ctx_size;
        memory_estimate.dynamic_context = true;
        apply_restricted_context_warning(memory_estimate, actual_ctx_size);

        LOG(INFO, "LlamaCpp") << "Dynamic context fit selected ctx_size=" << actual_ctx_size
                              << " (requested=" << ctx_size
                              << ", model_max_ctx=" << memory_estimate.model_max_context
                              << ", target=" << memory_estimate.target_context << ")" << std::endl;
    } else {
        memory_estimate = MemoryManager::estimate_llamacpp_memory(
            model_info, gguf_path, memory_backend, ctx_size, runtime_ram_limit);
        memory_estimate.dynamic_context = false;
        if (memory_estimate.hard_error) {
            throw MemoryPreflightException(memory_estimate.warning);
        }
        port_ = choose_port();
        int actual_ctx_size = start_with_ctx(ctx_size);
        memory_estimate.final_context = actual_ctx_size;
        apply_restricted_context_warning(memory_estimate, actual_ctx_size);
    }

    set_memory_estimate(memory_estimate);
    LOG(DEBUG, "LlamaCpp") << "Model loaded on port " << port_ << std::endl;
}

void LlamaCppServer::unload() {
    LOG(INFO, "LlamaCpp") << "Unloading model..." << std::endl;
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
