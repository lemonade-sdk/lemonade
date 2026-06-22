#include "lemon/backends/vllm_server.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backends/vllm_arg_resolver.h"
#include "lemon/model_manager.h"
#include "lemon/runtime_config.h"
#include "lemon/system_info.h"
#include "lemon/utils/http_client.h"
#include "lemon/utils/json_utils.h"
#include "lemon/utils/path_utils.h"
#include "lemon/utils/process_manager.h"
#include <lemon/utils/aixlog.hpp>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;
using namespace lemon::utils;

namespace lemon {
namespace backends {

static constexpr int64_t VLLM_MAX_TOKENS_PREFLIGHT_THRESHOLD = 8192;
static constexpr int64_t VLLM_TOKEN_FIT_RESERVE = 64;

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

static json with_legacy_token_limit(const json& request) {
    json modified_request = request;
    if (modified_request.contains("max_completion_tokens") && !modified_request.contains("max_tokens")) {
        modified_request["max_tokens"] = modified_request["max_completion_tokens"];
    }
    return modified_request;
}

json VLLMServer::prepare_openai_request(const json& request) {
    return with_legacy_token_limit(fit_openai_max_tokens_to_context(request));
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
    max_model_len_ = ctx_size;

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

    std::string vllm_model_config_path =
        utils::get_resource_path("resources/vllm_model_config.json");
    json vllm_model_config = json::object();
    if (fs::exists(utils::path_from_utf8(vllm_model_config_path))) {
        vllm_model_config = JsonUtils::load_from_file(vllm_model_config_path);
    } else {
        LOG(WARNING, "vLLM") << "vLLM model config not found at "
                             << vllm_model_config_path
                             << "; continuing with user vllm_args only" << std::endl;
    }
    VLLMArgResolution resolved_vllm_args =
        resolve_vllm_args(model_name, model_id, vllm_model_config, vllm_args);

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
    // Keep eager execution for consumer GPU inference; leave dtype selection to vLLM.
    args.push_back("--enforce-eager");
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
        // vLLM's AWQ kernels only support float16. Many AWQ repos still declare
        // bfloat16 in config.json, which makes vLLM abort with "torch.bfloat16 is
        // not supported for quantization method awq". Force float16 so AWQ models
        // load, unless the user already pinned a --dtype themselves.
        if (!resolved_vllm_args.has_dtype_arg) {
            LOG(DEBUG, "vLLM") << "Forcing --dtype float16 for AWQ" << std::endl;
            args.push_back("--dtype");
            args.push_back("float16");
        }
    } else if (!quant_method.empty()) {
        LOG(DEBUG, "vLLM") << "Detected quantization '" << quant_method
                           << "'; letting vLLM auto-select kernel" << std::endl;
    }

    // enable prompt caching
    args.push_back("--enable-prefix-caching");

    // Avoid vLLM's default gpu_memory_utilization=0.92 on shared-memory systems.
    // Keep this overridable through vllm_args for users that want another limit.
    if (!resolved_vllm_args.has_memory_budget_arg) {
        args.push_back("--kv-cache-memory-bytes");
        args.push_back("4G");
    }

    if (!resolved_vllm_args.args.empty()) {
        LOG(DEBUG, "vLLM") << "Adding model/user arguments from vLLM resolver" << std::endl;
        args.insert(args.end(), resolved_vllm_args.args.begin(), resolved_vllm_args.args.end());
    }

    LOG(INFO, "vLLM") << "Starting vllm-server on port " << get_backend_port() << "..." << std::endl;

    // Set environment variables
    std::vector<std::pair<std::string, std::string>> env_vars;

    // The vllm-server launcher script handles LD_LIBRARY_PATH for ROCm libs.
    // Set FLASH_ATTENTION_TRITON_AMD_ENABLE for ROCm flash attention.
    env_vars.push_back({"FLASH_ATTENTION_TRITON_AMD_ENABLE", "TRUE"});
    // Prevent system/user Python packages from leaking into the bundled vLLM environment
    env_vars.push_back({"PYTHONNOUSERSITE", "1"});

    // Start process
    bool inherit_output = (log_level_ == "info") || is_debug();
    set_process_handle(ProcessManager::start_process(executable, args, "", inherit_output, true, env_vars));

    // vLLM can take longer to start (loading model, compiling kernels)
    if (!wait_for_ready("/health", HttpClient::get_default_timeout())) {
        const ProcessHandle handle = consume_process_handle_for_cleanup();
        if (has_process_handle(handle)) {
            ProcessManager::stop_process(handle);
        }
        max_model_len_ = 0;
        std::string err = "vllm-server failed to start within timeout";
        // A common cause on gfx1151 is a kernel without the CWSR fix, which makes
        // any GPU dispatch hang or fault. Point users to the docs in that case.
        if (needs_gfx1151_cwsr_fix()) {
            err += ". Your kernel may be missing the gfx1151 CWSR fix — "
                   "see https://lemonade-server.ai/gfx1151_linux.html";
        }
        throw std::runtime_error(err);
    }

    LOG(DEBUG, "vLLM") << "Model loaded on port " << get_backend_port() << std::endl;
}

void VLLMServer::unload() {
    stop_backend_watchdog();
    LOG(INFO, "vLLM") << "Unloading model..." << std::endl;

    const ProcessHandle handle = consume_process_handle_for_cleanup();
    if (has_process_handle(handle)) {
        ProcessManager::stop_process(handle);
    }
    max_model_len_ = 0;
}

json VLLMServer::chat_completion(const json& request) {
    return forward_request("/v1/chat/completions", prepare_openai_request(request));
}

json VLLMServer::completion(const json& request) {
    return forward_request("/v1/completions", prepare_openai_request(request));
}

json VLLMServer::responses(const json& request) {
    return forward_request("/v1/responses", request);
}

void VLLMServer::forward_streaming_request(const std::string& endpoint,
                                           const std::string& request_body,
                                           httplib::DataSink& sink,
                                           bool sse,
                                           long timeout_seconds,
                                           TelemetryCallback telemetry_callback) {
    std::string body = request_body;
    const auto start = std::chrono::steady_clock::now();

    if (sse && (endpoint == "/v1/chat/completions" || endpoint == "/v1/completions")) {
        try {
            json request = prepare_openai_request(json::parse(request_body));
            json& stream_options = request["stream_options"];
            if (!stream_options.is_object()) {
                stream_options = json::object();
            }
            stream_options["include_usage"] = true;
            body = request.dump();
        } catch (...) {
            // Forward the original request if it cannot be parsed.
        }
    }

    Telemetry telemetry;
    bool has_telemetry = false;

    WrappedServer::forward_streaming_request(
        endpoint, body, sink, sse, timeout_seconds,
        [&telemetry, &has_telemetry](int input_tokens,
                                     int output_tokens,
                                     double time_to_first_token,
                                     double tokens_per_second) {
            has_telemetry = true;
            telemetry.input_tokens = input_tokens;
            telemetry.output_tokens = output_tokens;
            telemetry.time_to_first_token = time_to_first_token;
            telemetry.tokens_per_second = tokens_per_second;
        });

    if (has_telemetry) {
        if (sse && telemetry.output_tokens > 0 && telemetry.tokens_per_second <= 0.0) {
            const double elapsed_seconds = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - start).count();
            const double decode_seconds = elapsed_seconds - telemetry.time_to_first_token;
            const double tps_seconds = decode_seconds > 0.0 ? decode_seconds : elapsed_seconds;
            if (tps_seconds > 1e-6) {
                telemetry.tokens_per_second = telemetry.output_tokens / tps_seconds;
            }
        }

        if (telemetry_callback) {
            telemetry_callback(telemetry.input_tokens,
                               telemetry.output_tokens,
                               telemetry.time_to_first_token,
                               telemetry.tokens_per_second);
        }
    }
}

json VLLMServer::fit_openai_max_tokens_to_context(const json& request) {
    if (max_model_len_ <= 0) {
        return request;
    }

    bool has_max_completion_tokens = request.contains("max_completion_tokens") &&
        (request["max_completion_tokens"].is_number_integer() ||
         request["max_completion_tokens"].is_number_unsigned());
    bool has_max_tokens = request.contains("max_tokens") &&
        (request["max_tokens"].is_number_integer() ||
         request["max_tokens"].is_number_unsigned());
    if (!has_max_completion_tokens && !has_max_tokens) {
        return request;
    }

    int64_t requested_max_tokens = has_max_completion_tokens
        ? request["max_completion_tokens"].get<int64_t>()
        : request["max_tokens"].get<int64_t>();
    if (requested_max_tokens <= 0 ||
        requested_max_tokens <= VLLM_MAX_TOKENS_PREFLIGHT_THRESHOLD) {
        return request;
    }

    int64_t input_tokens = count_openai_prompt_tokens(request);
    if (input_tokens <= 0) {
        return request;
    }

    if (input_tokens >= max_model_len_) {
        return request;
    }

    int64_t available_output_tokens =
        std::max<int64_t>(1, max_model_len_ - input_tokens - VLLM_TOKEN_FIT_RESERVE);
    if (requested_max_tokens <= available_output_tokens) {
        return request;
    }

    json modified_request = request;
    if (has_max_completion_tokens) {
        modified_request["max_completion_tokens"] = available_output_tokens;
    }
    if (has_max_tokens) {
        modified_request["max_tokens"] = available_output_tokens;
    }
    LOG(INFO, "vLLM") << "Reduced OpenAI max tokens from " << requested_max_tokens
                      << " to " << available_output_tokens
                      << " so input_tokens (" << input_tokens
                      << ") fits max_model_len (" << max_model_len_
                      << ") with reserve (" << VLLM_TOKEN_FIT_RESERVE << ")" << std::endl;
    return modified_request;
}

int64_t VLLMServer::count_openai_prompt_tokens(const json& request) {
    json tokenize_request;
    tokenize_request["model"] = model_name_;
    if (request.contains("messages")) {
        tokenize_request["messages"] = request["messages"];
    } else if (request.contains("prompt")) {
        tokenize_request["prompt"] = request["prompt"];
    } else {
        return 0;
    }
    if (request.contains("tools")) {
        tokenize_request["tools"] = request["tools"];
    }
    if (request.contains("tool_choice")) {
        tokenize_request["tool_choice"] = request["tool_choice"];
    }

    // This is a synchronous backend round trip on the request path. It only
    // runs for oversized max-token requests so vLLM receives a context-safe
    // limit before generation or streaming begins.
    auto response = forward_request("/tokenize", tokenize_request);
    if (response.contains("error")) {
        LOG(DEBUG, "vLLM") << "Skipping max token fit; /tokenize returned error: "
                           << response.dump() << std::endl;
        return 0;
    }

    if (response.contains("count") &&
        (response["count"].is_number_integer() || response["count"].is_number_unsigned())) {
        return response["count"].get<int64_t>();
    }
    if (response.contains("token_count") &&
        (response["token_count"].is_number_integer() || response["token_count"].is_number_unsigned())) {
        return response["token_count"].get<int64_t>();
    }
    if (response.contains("tokens") && response["tokens"].is_array()) {
        return static_cast<int64_t>(response["tokens"].size());
    }
    if (response.contains("token_ids") && response["token_ids"].is_array()) {
        return static_cast<int64_t>(response["token_ids"].size());
    }

    LOG(DEBUG, "vLLM") << "Skipping max token fit; unrecognized /tokenize response: "
                       << response.dump() << std::endl;
    return 0;
}

} // namespace backends
} // namespace lemon
