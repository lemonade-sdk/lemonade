#include "lemon/backends/vllm_server.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/model_manager.h"
#include "lemon/runtime_config.h"
#include "lemon/system_info.h"
#include "lemon/utils/http_client.h"
#include "lemon/utils/process_manager.h"
#include <lemon/utils/aixlog.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;
using namespace lemon::utils;

namespace lemon {
namespace backends {

// Parse quantization_config.quant_method from a config.json body.
static std::string parse_quant_method(const std::string& config_json) {
    try {
        json j = json::parse(config_json);
        if (j.contains("quantization_config")) {
            const auto& qc = j["quantization_config"];
            if (qc.contains("quant_method") && qc["quant_method"].is_string()) {
                return qc["quant_method"].get<std::string>();
            }
        }
    } catch (const std::exception&) {
        // fall through
    }
    return "";
}

// Returns quantization_config.quant_method for the model, or empty string.
// First checks the HuggingFace hub cache; if config.json isn't there yet,
// fetches it over HTTP from huggingface.co directly. This ensures detection
// works on first load before vLLM has downloaded anything.
static std::string detect_quant_method(const std::string& model_id) {
    // 1. Check HF cache first (fast path)
    std::string hf_dir = "models--";
    for (char c : model_id) {
        if (c == '/') hf_dir += "--";
        else hf_dir += c;
    }

    const char* home = std::getenv("HOME");
    if (home) {
        fs::path snapshots = fs::path(home) / ".cache" / "huggingface" / "hub" / hf_dir / "snapshots";
        if (fs::exists(snapshots)) {
            for (const auto& entry : fs::directory_iterator(snapshots)) {
                if (!entry.is_directory()) continue;
                fs::path cfg = entry.path() / "config.json";
                if (!fs::exists(cfg)) continue;
                std::ifstream f(cfg);
                std::stringstream buf;
                buf << f.rdbuf();
                std::string result = parse_quant_method(buf.str());
                if (!result.empty()) return result;
            }
        }
    }

    // 2. Fetch directly from HF
    std::string url = "https://huggingface.co/" + model_id + "/resolve/main/config.json";
    auto resp = HttpClient::get(url);
    if (resp.status_code == 200) {
        return parse_quant_method(resp.body);
    }

    LOG(DEBUG, "vLLM") << "Could not fetch config.json for " << model_id
                       << " (http " << resp.status_code << "); skipping quant detection" << std::endl;
    return "";
}

InstallParams VLLMServer::get_install_params(const std::string& backend, const std::string& version) {
    InstallParams params;

    if (backend == "rocm") {
        params.repo = "lemonade-sdk/vllm-rocm";
        std::string target_arch = SystemInfo::get_rocm_arch();
        if (target_arch.empty()) {
            throw std::runtime_error(
                SystemInfo::get_unsupported_backend_error("vllm", "rocm")
            );
        }
#ifdef __linux__
        // One release per GPU target since 0.19.1: release tag is
        // {version}-{target_arch}, e.g. vllm0.20.1-rocm7.12.0-gfx1151.
        std::string release_tag = version + "-" + target_arch;
        params.version_override = release_tag;
        params.filename = release_tag + "-x64.tar.gz";
#else
        throw std::runtime_error("vLLM ROCm is only supported on Linux");
#endif
    } else {
        throw std::runtime_error("vLLM backend '" + backend + "' is not supported. Supported: rocm");
    }

    return params;
}

VLLMServer::VLLMServer(const std::string& log_level, ModelManager* model_manager, BackendManager* backend_manager)
    : WrappedServer("vllm-server", log_level, model_manager, backend_manager) {
}

VLLMServer::~VLLMServer() {
    unload();
}

void VLLMServer::load(const std::string& model_name,
                     const ModelInfo& model_info,
                     const RecipeOptions& options,
                     bool do_not_upgrade) {
    LOG(INFO, "vLLM") << "Loading model: " << model_name << std::endl;

    std::string vllm_backend = options.get_option("vllm_backend");
    std::string vllm_args = options.get_option("vllm_args");
    int ctx_size = options.get_option("ctx_size");

    RuntimeConfig::validate_backend_choice("vllm", vllm_backend);

    // Install vllm-server if needed
    backend_manager_->install_backend(SPEC.recipe, vllm_backend);

    // vLLM uses HuggingFace model names, not local file paths.
    // The checkpoint field in server_models.json is the HF model ID.
    std::string model_id = model_info.checkpoint();
    if (model_id.empty()) {
        throw std::runtime_error("Model checkpoint (HuggingFace ID) not found for: " + model_name);
    }

    LOG(DEBUG, "vLLM") << "Using model: " << model_id << std::endl;

    // Choose port
    port_ = choose_port();

    // Get executable path
    std::string executable = BackendUtils::get_backend_binary_path(SPEC, vllm_backend);

    // Build command line arguments
    std::vector<std::string> args;
    args.push_back("--model");
    args.push_back(model_id);
    args.push_back("--port");
    args.push_back(std::to_string(port_));
    args.push_back("--host");
    args.push_back("127.0.0.1");
    // Serve using the Lemonade model name so forwarded requests match
    args.push_back("--served-model-name");
    args.push_back(model_name);
    // Default args for consumer GPU inference
    args.push_back("--enforce-eager");
    args.push_back("--dtype");
    args.push_back("float16");
    // Pass ctx_size through to vllm-server's --max-model-len. Trust the
    // user's value verbatim; the global default lives in defaults.json
    // (same as llamacpp). Larger values raise KV-cache memory and Triton
    // JIT compile time.
    args.push_back("--max-model-len");
    args.push_back(std::to_string(ctx_size));
    // Detect the actual quantization method from config.json rather than guessing
    // from the model name. Repos named "...-AWQ" sometimes use compressed-tensors,
    // GPTQ, etc. and forcing --quantization awq would fail the load.
    // For AWQ specifically we force the 'awq' kernel because vLLM's default
    // awq_marlin is very slow on consumer GPUs (2 tok/s -> 12 tok/s).
    std::string quant_method = detect_quant_method(model_id);
    if (quant_method == "awq") {
        LOG(DEBUG, "vLLM") << "Detected AWQ; forcing --quantization awq" << std::endl;
        args.push_back("--quantization");
        args.push_back("awq");
    } else if (!quant_method.empty()) {
        LOG(DEBUG, "vLLM") << "Detected quantization '" << quant_method
                           << "'; letting vLLM auto-select kernel" << std::endl;
    }

    // Append custom vllm_args if provided
    if (!vllm_args.empty()) {
        LOG(DEBUG, "vLLM") << "Adding custom arguments: " << vllm_args << std::endl;
        std::istringstream iss(vllm_args);
        std::string arg;
        while (iss >> arg) {
            args.push_back(arg);
        }
    }

    LOG(INFO, "vLLM") << "Starting vllm-server on port " << port_ << "..." << std::endl;

    // Set environment variables
    std::vector<std::pair<std::string, std::string>> env_vars;

    // The vllm-server launcher script handles LD_LIBRARY_PATH for ROCm libs.
    // Set FLASH_ATTENTION_TRITON_AMD_ENABLE for ROCm flash attention.
    env_vars.push_back({"FLASH_ATTENTION_TRITON_AMD_ENABLE", "TRUE"});
    // Prevent system/user Python packages from leaking into the bundled vLLM environment
    env_vars.push_back({"PYTHONNOUSERSITE", "1"});

    // Start process
    bool inherit_output = (log_level_ == "info") || is_debug();
    process_handle_ = ProcessManager::start_process(executable, args, "", inherit_output, true, env_vars);

    // vLLM can take longer to start (loading model, compiling kernels)
    if (!wait_for_ready("/health", 600)) {
        ProcessManager::stop_process(process_handle_);
        process_handle_ = {nullptr, 0};
        std::string err = "vllm-server failed to start within timeout";
        // A common cause on gfx1151 is a kernel without the CWSR fix, which makes
        // any GPU dispatch hang or fault. Point users to the docs in that case.
        if (needs_gfx1151_cwsr_fix()) {
            err += ". Your kernel may be missing the gfx1151 CWSR fix — "
                   "see https://lemonade-server.ai/gfx1151_linux.html";
        }
        throw std::runtime_error(err);
    }

    LOG(DEBUG, "vLLM") << "Model loaded on port " << port_ << std::endl;
}

void VLLMServer::unload() {
    LOG(INFO, "vLLM") << "Unloading model..." << std::endl;
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

json VLLMServer::chat_completion(const json& request) {
    return forward_request("/v1/chat/completions", request);
}

json VLLMServer::completion(const json& request) {
    return forward_request("/v1/completions", request);
}

json VLLMServer::responses(const json& request) {
    return forward_request("/v1/responses", request);
}

} // namespace backends
} // namespace lemon
