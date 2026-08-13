#include "lemon/backends/vllm/vllm_server.h"
#include "lemon/backends/vllm/vllm.h"
#include "lemon/backends/backend_registry.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backends/hf_cache_util.h"
#include "lemon/backends/vllm/vllm_arg_resolver.h"
#include "lemon/model_manager.h"
#include "lemon/model_registry.h"
#include "lemon/runtime_config.h"
#include "lemon/system_info.h"
#include "lemon/utils/http_client.h"
#include "lemon/utils/json_utils.h"
#include "lemon/utils/path_utils.h"
#include "lemon/utils/process_manager.h"
#include <lemon/utils/aixlog.hpp>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <regex>
#include <sstream>

namespace fs = std::filesystem;
using namespace lemon::utils;

namespace lemon {
namespace backends {

static constexpr int64_t VLLM_MAX_TOKENS_PREFLIGHT_THRESHOLD = 8192;

static std::string registry_repo_id_from_checkpoint(const std::string& checkpoint) {
    const auto separator = checkpoint.find(':');
    return separator == std::string::npos ? checkpoint : checkpoint.substr(0, separator);
}

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

static void copy_if_present(json& target, const json& source, const char* key) {
    if (source.contains(key)) {
        target[key] = source[key];
    }
}

static void merge_chat_template_kwarg(json& target, const json& source, const char* key) {
    if (!source.contains(key)) {
        return;
    }
    if (!target.contains("chat_template_kwargs")) {
        target["chat_template_kwargs"] = json::object();
    }
    if (target["chat_template_kwargs"].is_object() &&
        !target["chat_template_kwargs"].contains(key)) {
        target["chat_template_kwargs"][key] = source[key];
    }
}

static void add_reasoning_effort_template_kwargs(json& target, const json& source) {
    merge_chat_template_kwarg(target, source, "reasoning_effort");
    if (!source.contains("reasoning_effort") || !source["reasoning_effort"].is_string() ||
        !target["chat_template_kwargs"].is_object() ||
        target["chat_template_kwargs"].contains("enable_thinking")) {
        return;
    }
    target["chat_template_kwargs"]["enable_thinking"] =
        source["reasoning_effort"].get<std::string>() != "none";
}

static void backup_invalid_vllm_model_config(const std::string& config_path) {
    fs::path backup_path = utils::path_from_utf8(config_path);
    backup_path += ".corrupted";

    std::error_code ec;
    fs::rename(utils::path_from_utf8(config_path), backup_path, ec);
    if (!ec) {
        LOG(WARNING, "vLLM") << "Renamed invalid vLLM model config to "
                             << utils::path_to_utf8(backup_path) << std::endl;
    }
}

static json load_vllm_model_config(const std::string& config_path,
                                   const std::string& model_name,
                                   const std::string& model_id) {
    const std::string fallback_message = "; continuing with user vllm_args only";
    if (!fs::exists(utils::path_from_utf8(config_path))) {
        LOG(WARNING, "vLLM") << "vLLM model config not found at "
                             << config_path << fallback_message << std::endl;
        return json::object();
    }

    json config;
    try {
        config = JsonUtils::load_from_file(config_path);
    } catch (const std::exception& e) {
        LOG(WARNING, "vLLM") << "Ignoring unreadable vLLM model config at "
                             << config_path << ": " << e.what()
                             << fallback_message << std::endl;
        backup_invalid_vllm_model_config(config_path);
        return json::object();
    }

    try {
        (void)resolve_vllm_args(model_name, model_id, config, "");
    } catch (const std::exception& e) {
        LOG(WARNING, "vLLM") << "Ignoring invalid vLLM model config at "
                             << config_path << ": " << e.what()
                             << fallback_message << std::endl;
        backup_invalid_vllm_model_config(config_path);
        return json::object();
    }

    return config;
}

json VLLMServer::prepare_openai_request(const json& request) {
    return JsonUtils::with_legacy_max_tokens_alias(fit_openai_max_tokens_to_context(request));
}

// Returns quantization_config.quant_method for the model, or empty string.
// Prefer the Lemonade-managed active snapshot. Hugging Face models retain the
// historical cache/network fallback; ModelScope models are always loaded from
// their normalized local snapshot and must never fall back to Hugging Face.
static std::string detect_quant_method(const std::string& model_id,
                                       const fs::path& local_snapshot,
                                       RemoteRegistrySource registry_source) {
    if (!local_snapshot.empty()) {
        fs::path cfg = local_snapshot / "config.json";
        if (fs::exists(cfg)) {
            std::ifstream f(cfg);
            std::stringstream buf;
            buf << f.rdbuf();
            std::string result = parse_quant_method(buf.str());
            if (!result.empty()) return result;
        }
    }

    if (registry_source != RemoteRegistrySource::HuggingFace) {
        return "";
    }

    // Check the historical HF cache location (fast path).
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

    // Fetch directly from HF only for Hugging Face registrations.
    std::string url = "https://huggingface.co/" + model_id + "/resolve/main/config.json";
    auto resp = HttpClient::get(url);
    if (resp.status_code == 200) {
        return parse_quant_method(resp.body);
    }

    LOG(DEBUG, "vLLM") << "Could not fetch config.json for " << model_id
                       << " (http " << resp.status_code << "); skipping quant detection" << std::endl;
    return "";
}

static std::string with_prepended_pythonpath(const fs::path& dir) {
    const char separator = ':';
    const char* current = std::getenv("PYTHONPATH");
    return dir.string() + ((current && current[0] != '\0')
        ? std::string(1, separator) + current
        : "");
}

static fs::path create_vllm_rocm_shim_dir() {
    fs::path runtime_base = path_from_utf8(get_runtime_dir());
    std::random_device rd;
    std::uniform_int_distribution<unsigned int> dis(0, 0xFFFFFF);

    std::error_code ec;
    for (int attempt = 0; attempt < 8; ++attempt) {
        auto nonce = static_cast<unsigned long long>(
            std::chrono::steady_clock::now().time_since_epoch().count());

        std::ostringstream suffix;
        suffix << "vllm-rocm-shim-" << nonce << "-" << std::hex << dis(rd);

        fs::path candidate = runtime_base / suffix.str();

        ec.clear();
        if (fs::create_directory(candidate, ec)) {
            return candidate;
        }
    }

    throw std::runtime_error("Failed to create temporary directory for vLLM ROCm startup shim");
}

static fs::path write_vllm_rocm_startup_shim() {
    fs::path dir = create_vllm_rocm_shim_dir();
    fs::path file = dir / "sitecustomize.py";

    std::ofstream out(file, std::ios::trunc);
    if (!out.is_open()) {
        std::error_code ec;
        fs::remove_all(dir, ec);
        throw std::runtime_error("Unable to write vLLM ROCm startup shim: " + file.string());
    }

    out << R"PY(# Auto-generated by Lemonade for the vLLM ROCm backend.
import sys

try:
    import vllm.utils.import_utils as _vllm_import_utils

    def _disabled_import_pynvml():
        raise ModuleNotFoundError(
            "vLLM NVML probe disabled by Lemonade for the ROCm backend"
        )

    _vllm_import_utils.import_pynvml = _disabled_import_pynvml
except (ImportError, AttributeError) as exc:
    print(
        f"Lemonade warning: unable to disable vLLM NVML probe: {exc}",
        file=sys.stderr,
    )
)PY";

    if (!out.good()) {
        std::error_code ec;
        fs::remove_all(dir, ec);
        throw std::runtime_error("Failed to write vLLM ROCm startup shim: " + file.string());
    }

    return dir;
}

static void cleanup_vllm_rocm_shim_dir(fs::path& shim_dir) {
    if (shim_dir.empty()) {
        return;
    }

    std::error_code ec;
    fs::remove_all(shim_dir, ec);
    if (ec) {
        LOG(WARNING, "vLLM") << "Failed to remove vLLM ROCm startup shim directory: "
                             << shim_dir << " (" << ec.message() << ")"
                             << std::endl;
    }
    shim_dir.clear();
}

static void configure_vllm_rocm_env(
    const std::string& backend,
    std::vector<std::pair<std::string, std::string>>& env_vars,
    fs::path& shim_dir) {
    if (backend != "rocm") {
        return;
    }

    cleanup_vllm_rocm_shim_dir(shim_dir);

    // Avoid vLLM activating both CUDA and ROCm platform plugins on hybrid
    // AMD/NVIDIA hosts. Do not set CUDA_VISIBLE_DEVICES: ROCm PyTorch exposes
    // HIP devices through torch.cuda/torch.accelerator.
    shim_dir = write_vllm_rocm_startup_shim();
    env_vars.push_back({"PYTHONPATH", with_prepended_pythonpath(shim_dir)});
}

InstallParams VLLMServer::get_install_params(const std::string& backend, const std::string& version) {
    InstallParams params;

    if (backend == "rocm") {
        params.repo = "lemonade-sdk/vllm-rocm";
        std::string target_arch =
            SystemInfo::rocm_asset_family(SystemInfo::get_rocm_arch());
        if (target_arch.empty()) {
            throw std::runtime_error(
                SystemInfo::get_unsupported_backend_error("vllm", "rocm")
            );
        }
#ifdef __linux__
        // The per-arch override replaces ONLY the builtin default base, so an explicit
        // vllm.rocm_bin pin is not silently clobbered on an MI300X host.
        std::string arch_override = SystemInfo::vllm_rocm_version_override(target_arch);
        std::string default_pin = BackendUtils::get_backend_version("vllm", "rocm");
        const bool on_builtin_default = version.empty() || version == default_pin;
        const std::string& effective_version =
            (!arch_override.empty() && on_builtin_default) ? arch_override : version;
        // One release per GPU target since 0.19.1 (vllm0.20.1-rocm7.12.0-gfx1151): a bare
        // base gets the detected suffix appended. An already-suffixed pin is rejected
        // unless it matches, so a cross-arch pin cannot install against the wrong line.
        static const std::regex arch_suffix_re("-(gfx[0-9a-fA-FxX]+)$");
        std::smatch arch_suffix_match;
        std::string release_tag;
        if (std::regex_search(effective_version, arch_suffix_match, arch_suffix_re)) {
            const std::string pinned_arch = arch_suffix_match[1].str();
            if (pinned_arch != target_arch) {
                throw std::runtime_error(
                    "vLLM ROCm pin '" + effective_version + "' targets " + pinned_arch +
                    " but this host is " + target_arch +
                    "; pin a " + target_arch +
                    " release (vllm.rocm_bin) or unset it to use the default.");
            }
            release_tag = effective_version;
        } else {
            release_tag = effective_version + "-" + target_arch;
        }
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

    backend_manager_->install_backend(vllm::spec()->recipe, vllm_backend);

    std::string model_id = model_info.checkpoint();
    if (model_id.empty()) {
        throw std::runtime_error("Model checkpoint (registry ID) not found for: " + model_name);
    }

    const RemoteRegistrySource registry_source =
        parse_remote_registry_source(model_info.registry_source);
    std::string model_target = model_id;
    fs::path active_snapshot;

    // vLLM can resolve Hugging Face IDs itself, but it has no native knowledge
    // of Lemonade's ModelScope cache namespace. Point it at the normalized
    // active snapshot so it stays offline and uses the exact revision Lemonade
    // recorded for update checks.
    if (registry_source == RemoteRegistrySource::ModelScope) {
        if (model_manager_ == nullptr) {
            throw std::runtime_error("Model manager is unavailable while resolving ModelScope cache");
        }
        const std::string repo_id = registry_repo_id_from_checkpoint(model_id);
        const fs::path repo_cache = path_from_utf8(model_manager_->get_hf_cache_dir()) /
            hf_cache::repo_id_to_cache_dir_name(repo_id, model_info.registry_source);
        active_snapshot = hf_cache::active_snapshot_path(repo_cache);
        if (active_snapshot.empty() || !fs::exists(active_snapshot)) {
            throw std::runtime_error(
                "ModelScope snapshot is not downloaded or refs/main is invalid for: " + model_name);
        }
        model_target = path_to_utf8(active_snapshot);
    }

    LOG(DEBUG, "vLLM") << "Using model target: " << model_target << std::endl;

    std::string vllm_model_config_path =
        utils::get_resource_path("resources/vllm_model_config.json");
    json vllm_model_config =
        load_vllm_model_config(vllm_model_config_path, model_name, model_id);
    VLLMArgResolution resolved_vllm_args =
        resolve_vllm_args(model_name, model_id, vllm_model_config, vllm_args);

    port_ = choose_port();

    std::string executable = BackendUtils::get_backend_binary_path(*vllm::spec(), vllm_backend);

    std::vector<std::string> args;
    args.push_back("--model");
    args.push_back(model_target);
    args.push_back("--port");
    args.push_back(std::to_string(port_));
    args.push_back("--host");
    args.push_back("127.0.0.1");
    // Serve using the Lemonade model name so forwarded requests match
    args.push_back("--served-model-name");
    args.push_back(model_name);
    // Keep eager execution for consumer GPU inference; leave dtype selection to vLLM.
    // Discrete-HBM parts skip it: eager costs decode throughput for no stability gain.
    const DeviceClassLaunchPolicy launch_policy = device_class_launch_policy(
        SystemInfo::get_rocm_arch(), resolved_vllm_args.has_memory_budget_arg,
        resolved_vllm_args.has_enforce_eager);
    if (launch_policy.enforce_eager) {
        args.push_back("--enforce-eager");
    }
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
    std::string quant_method = detect_quant_method(
        model_id, active_snapshot, registry_source);
    if (quant_method == "awq" && launch_policy.force_awq_kernel) {
        if (!resolved_vllm_args.has_quantization_arg) {
            LOG(DEBUG, "vLLM") << "Detected AWQ; forcing --quantization awq" << std::endl;
            args.push_back("--quantization");
            args.push_back("awq");
        } else {
            LOG(DEBUG, "vLLM") << "Detected AWQ; using resolved --quantization "
                               << resolved_vllm_args.quantization_arg << std::endl;
        }
        // vLLM's AWQ kernels only support float16. Many AWQ repos still declare
        // bfloat16 in config.json, which makes vLLM abort with "torch.bfloat16 is
        // not supported for quantization method awq". Force float16 so AWQ models
        // load, unless the user already pinned a --dtype themselves.
        bool effective_awq_quantization =
            !resolved_vllm_args.has_quantization_arg ||
            resolved_vllm_args.quantization_arg == "awq";
        if (effective_awq_quantization && !resolved_vllm_args.has_dtype_arg) {
            LOG(DEBUG, "vLLM") << "Forcing --dtype float16 for AWQ" << std::endl;
            args.push_back("--dtype");
            args.push_back("float16");
        }
    } else if (!quant_method.empty()) {
        LOG(DEBUG, "vLLM") << "Detected quantization '" << quant_method
                           << "'; letting vLLM auto-select kernel" << std::endl;
    }

    args.push_back("--enable-prefix-caching");

    // Avoid vLLM's default gpu_memory_utilization=0.92 on shared-memory systems.
    // Keep this overridable through vllm_args for users that want another limit.
    // Discrete-HBM GPUs get vLLM's native budgeting instead of a fixed cap.
    if (launch_policy.cap_kv_cache) {
        args.push_back("--kv-cache-memory-bytes");
        args.push_back("4G");
    }

    // Emitted as its own argv element, never through vllm_args: that tokenizer strips quotes
    // and would corrupt the JSON.
    if (!resolved_vllm_args.speculative_config.empty()) {
        LOG(DEBUG, "vLLM") << "Enabling speculative decoding: "
                           << resolved_vllm_args.speculative_config << std::endl;
        args.push_back("--speculative-config");
        args.push_back(resolved_vllm_args.speculative_config);
    }

    if (!resolved_vllm_args.args.empty()) {
        LOG(DEBUG, "vLLM") << "Adding model/user arguments from vLLM resolver" << std::endl;
        args.insert(args.end(), resolved_vllm_args.args.begin(), resolved_vllm_args.args.end());
    }

    LOG(INFO, "vLLM") << "Starting vllm-server on port " << get_backend_port() << "..." << std::endl;

    std::vector<std::pair<std::string, std::string>> env_vars;

    configure_vllm_rocm_env(vllm_backend, env_vars, rocm_shim_dir_);

    // Enable ROCm flash attention (the launcher script handles LD_LIBRARY_PATH).
    env_vars.push_back({"FLASH_ATTENTION_TRITON_AMD_ENABLE", "TRUE"});
    // Prevent system/user Python packages from leaking into the bundled vLLM environment
    env_vars.push_back({"PYTHONNOUSERSITE", "1"});

    bool inherit_output = (log_level_ == "info") || is_debug();
    set_process_handle(ProcessManager::start_process(executable, args, "", inherit_output, true, env_vars));

    // vLLM can take longer to start (loading model, compiling kernels)
    if (!wait_for_ready("/health", HttpClient::get_default_timeout())) {
        const ProcessHandle handle = consume_process_handle_for_cleanup();
        if (has_process_handle(handle)) {
            ProcessManager::stop_process(handle);
        }
        cleanup_vllm_rocm_shim_dir(rocm_shim_dir_);
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
    cleanup_vllm_rocm_shim_dir(rocm_shim_dir_);
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
    std::string telemetry_error = "";

    WrappedServer::forward_streaming_request(
        endpoint, body, sink, sse, timeout_seconds,
        [&telemetry, &has_telemetry, &telemetry_error](int input_tokens,
                                     int output_tokens,
                                     double time_to_first_token,
                                     double tokens_per_second,
                                     const std::string& error_message) {
            has_telemetry = true;
            telemetry.input_tokens = input_tokens;
            telemetry.output_tokens = output_tokens;
            telemetry.time_to_first_token = time_to_first_token;
            telemetry.tokens_per_second = tokens_per_second;
            telemetry_error = error_message;
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
                               telemetry.tokens_per_second,
                               telemetry_error);
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
    const int64_t preflight_threshold =
        std::min<int64_t>(VLLM_MAX_TOKENS_PREFLIGHT_THRESHOLD, max_model_len_);
    if (requested_max_tokens <= 0 ||
        requested_max_tokens <= preflight_threshold) {
        return request;
    }

    int64_t input_tokens = count_openai_prompt_tokens(request);
    if (input_tokens <= 0) {
        return request;
    }

    int64_t available_output_tokens = max_model_len_ - input_tokens;
    if (available_output_tokens <= 0) {
        return request;
    }

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
                      << ") fits max_model_len (" << max_model_len_ << ")" << std::endl;
    return modified_request;
}

int64_t VLLMServer::count_openai_prompt_tokens(const json& request) {
    json tokenize_request;
    tokenize_request["model"] = model_name_;
    if (request.contains("messages")) {
        tokenize_request["messages"] = request["messages"];
        copy_if_present(tokenize_request, request, "add_generation_prompt");
        copy_if_present(tokenize_request, request, "continue_final_message");
        copy_if_present(tokenize_request, request, "add_special_tokens");
        copy_if_present(tokenize_request, request, "chat_template");
        copy_if_present(tokenize_request, request, "chat_template_kwargs");
        copy_if_present(tokenize_request, request, "media_io_kwargs");
        copy_if_present(tokenize_request, request, "mm_processor_kwargs");
        copy_if_present(tokenize_request, request, "tools");
        merge_chat_template_kwarg(tokenize_request, request, "documents");
        add_reasoning_effort_template_kwargs(tokenize_request, request);
    } else if (request.contains("prompt")) {
        tokenize_request["prompt"] = request["prompt"];
        copy_if_present(tokenize_request, request, "add_special_tokens");
    } else {
        return 0;
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

std::map<std::string, nlohmann::json> parse_vllm_metrics_text(const std::string& body) {
    std::map<std::string, nlohmann::json> telemetry_attrs;
    std::istringstream stream(body);
    std::string line;
    std::vector<std::pair<std::string, std::string>> keys = {
        {"vllm:gpu_cache_usage_factor", "llm.vllm.gpu_cache_usage_factor"},
        {"vllm:cpu_cache_usage_factor", "llm.vllm.cpu_cache_usage_factor"},
        {"vllm:num_requests_waiting", "llm.vllm.num_requests_waiting"},
        {"vllm:num_requests_running", "llm.vllm.num_requests_running"},
        {"vllm:num_requests_swapped", "llm.vllm.num_requests_swapped"}
    };
    while (std::getline(stream, line)) {
        for (const auto& [prom_key, span_key] : keys) {
            if (line.rfind(prom_key, 0) == 0) {
                size_t last_space = line.find_last_of(" \t");
                if (last_space != std::string::npos) {
                    try {
                        double val = std::stod(line.substr(last_space + 1));
                        telemetry_attrs[span_key] = val;
                    } catch (...) {}
                }
                break;
            }
        }
    }
    return telemetry_attrs;
}

std::map<std::string, nlohmann::json> VLLMServer::get_additional_telemetry() {
    if (get_backend_port() <= 0) {
        return {};
    }

    std::string url = "http://127.0.0.1:" + std::to_string(get_backend_port()) + "/metrics";
    try {
        auto response = utils::HttpClient::get(url, {}, 1,
                                               utils::HttpSecurityPolicy::TrustedLoopback);
        if (response.status_code == 200) {
            return parse_vllm_metrics_text(response.body);
        }
    } catch (const std::exception& e) {
        LOG(DEBUG, "vLLM") << "Failed to fetch metrics from vllm-server: " << e.what() << std::endl;
    } catch (...) {
        LOG(DEBUG, "vLLM") << "Failed to fetch metrics from vllm-server: unknown error" << std::endl;
    }
    return {};
}

std::string VLLMServer::get_additional_telemetry_url() const {
    if (get_backend_port() <= 0) {
        return "";
    }
    return "http://127.0.0.1:" + std::to_string(get_backend_port()) + "/metrics";
}

std::function<std::map<std::string, nlohmann::json>(const std::string&)> VLLMServer::get_additional_telemetry_parser() const {
    return [](const std::string& body) {
        return parse_vllm_metrics_text(body);
    };
}

} // namespace backends
} // namespace lemon

namespace lemon {
namespace backends {
namespace vllm {

std::unique_ptr<WrappedServer> create(const BackendContext& ctx) {
    return make_server<VLLMServer>(ctx);
}


const BackendSpec* spec() { return make_spec<VLLMServer>(descriptor, /*split=*/true); }
const BackendOps* ops() { return default_backend_ops(); }
}  // namespace vllm
}  // namespace backends
}  // namespace lemon
