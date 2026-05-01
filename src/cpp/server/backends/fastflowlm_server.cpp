#include "lemon/backends/fastflowlm_server.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/system_info.h"
#include "lemon/error_types.h"
#include "lemon/runtime_config.h"
#include "lemon/memory_manager.h"
#include "lemon/utils/process_manager.h"
#include "lemon/utils/http_client.h"
#include "lemon/utils/path_utils.h"
#include "lemon/utils/json_utils.h"
#include <iostream>
#include <filesystem>
#include <cstdlib>
#include <thread>
#include <chrono>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <limits>
#include <cmath>
#include <lemon/utils/aixlog.hpp>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/wait.h>
#endif

namespace fs = std::filesystem;

namespace lemon {
namespace backends {

// URL to direct users to for driver updates
static const std::string DRIVER_INSTALL_URL = "https://lemonade-server.ai/driver_install.html";

namespace {
constexpr uint64_t kKiB = 1024ULL;
constexpr uint64_t kGiB = 1024ULL * 1024ULL * kKiB;

uint64_t saturating_add_local(uint64_t a, uint64_t b) {
    if (std::numeric_limits<uint64_t>::max() - a < b) {
        return std::numeric_limits<uint64_t>::max();
    }
    return a + b;
}

uint64_t model_weight_bytes_from_info(const ModelInfo& model_info) {
    if (model_info.size > 0.0) {
        return static_cast<uint64_t>(std::ceil(model_info.size * static_cast<double>(kGiB)));
    }
    return 0;
}

uint64_t estimate_flm_npu_context_guard_bytes_per_token(const ModelInfo& model_info) {
    // FLM does not expose a llama.cpp-style memory fit/preflight API today.
    // Do not use this as a hard estimator. It is only a soft guardrail for the
    // first ctx-len attempt. FLM process success/failure remains the final
    // signal. Keep the values modest to avoid falsely restricting models
    // that FLM can load successfully at the requested context target.
    const uint64_t weight = model_weight_bytes_from_info(model_info);
    if (weight == 0) return 16ULL * kKiB;
    if (weight <= 2ULL * kGiB) return 8ULL * kKiB;
    if (weight <= 4ULL * kGiB) return 12ULL * kKiB;
    if (weight <= 8ULL * kGiB) return 16ULL * kKiB;
    return 24ULL * kKiB;
}

int round_context_down_to_step(uint64_t tokens) {
    if (tokens <= static_cast<uint64_t>(MemoryManager::kProbeContext)) {
        return MemoryManager::kProbeContext;
    }
    tokens = (tokens / 1024ULL) * 1024ULL;
    if (tokens > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
        return std::numeric_limits<int>::max();
    }
    return static_cast<int>(tokens);
}

int calculate_flm_npu_preflight_context(const ModelMemoryEstimate& estimate,
                                        int requested_context,
                                        int64_t ram_limit_mib) {
    const int requested = std::max(MemoryManager::kProbeContext, requested_context);
    const uint64_t per_token = std::max<uint64_t>(estimate.kv_cache_bytes_per_token, 1);
    const SystemMemoryProbe probe = MemoryManager::probe_system_memory(ram_limit_mib);

    if (probe.ram_limit_bytes == 0 || probe.ram_limit_bytes >= probe.available_bytes) {
        return requested;
    }

    // FLM currently has no dry-run/fit API. Use a single optimistic preflight
    // cap so we do not start high and walk down through repeated unloads. The
    // process itself remains the final authority: if the cap is still too high,
    // the normal start failure path can try a smaller context once.
    const uint64_t base_floor = saturating_add_local(
        estimate.weight_bytes, 256ULL * 1024ULL * 1024ULL);
    const uint64_t tolerance = std::min<uint64_t>(
        512ULL * 1024ULL * 1024ULL,
        std::max<uint64_t>(128ULL * 1024ULL * 1024ULL, probe.ram_limit_bytes / 10ULL));
    const uint64_t allowed = saturating_add_local(probe.ram_limit_bytes, tolerance);
    if (base_floor >= allowed) {
        return MemoryManager::kProbeContext;
    }

    const uint64_t context_budget = allowed - base_floor;
    uint64_t max_context_by_budget = context_budget / per_token;
    if (max_context_by_budget >= static_cast<uint64_t>(requested)) {
        return requested;
    }

    return std::min(requested, round_context_down_to_step(max_context_by_budget));
}
} // namespace

InstallParams FastFlowLMServer::get_install_params(const std::string& backend, const std::string& version) {
    InstallParams params;

    if (backend == "system") {
        return params;
    }

    params.repo = "FastFlowLM/FastFlowLM";

    // Release asset filenames use bare version numbers (no 'v' prefix)
    std::string bare_version = version;
    if (!bare_version.empty() && bare_version[0] == 'v') {
        bare_version = bare_version.substr(1);
    }

#ifdef _WIN32
    params.filename = "fastflowlm_" + bare_version + "_windows_amd64.zip";
#else
    // On Linux, FLM must be installed as a system package by the user.
    // The FLM .deb bundles non-portable libraries (libxrt, ffmpeg) that
    // require system-level installation. Auto-install is Windows-only.
    throw std::runtime_error(
        "FLM auto-install is only supported on Windows. "
        "On Linux, install FLM manually: "
        "https://github.com/FastFlowLM/FastFlowLM/releases/tag/" + version);
#endif

    return params;
}

FastFlowLMServer::FastFlowLMServer(const std::string& log_level, ModelManager* model_manager,
                                   BackendManager* backend_manager)
    : WrappedServer("FastFlowLM", log_level, model_manager, backend_manager) {
}

FastFlowLMServer::~FastFlowLMServer() {
    unload();
}

std::string FastFlowLMServer::download_model(const std::string& checkpoint, bool do_not_upgrade) {
    LOG(INFO, "FastFlowLM") << "Pulling model with FLM: " << checkpoint << std::endl;

    // Use flm pull command to download the model
    std::string flm_path = get_flm_path();
    if (flm_path.empty()) {
        throw std::runtime_error("FLM not found");
    }

    std::vector<std::string> args = {"pull", checkpoint};
    if (!do_not_upgrade) {
        args.push_back("--force");
    }

    LOG(INFO, "ProcessManager") << "Starting process: \"" << flm_path << "\"";
    for (const auto& arg : args) {
        LOG(INFO, "ProcessManager") << " \"" << arg << "\"";
    }
    LOG(INFO, "ProcessManager") << std::endl;

    // Run flm pull command (with debug output if enabled)
    auto handle = utils::ProcessManager::start_process(flm_path, args, "", is_debug());

    // Wait for process to complete (handles both fast exits and long downloads)
    // NOTE: On Linux, is_running() reaps the process via waitpid(), making the
    // exit code unavailable to get_exit_code(). Use WaitForSingleObject/waitpid
    // directly instead of the is_running/get_exit_code combo.
    int timeout_seconds = 300; // 5 minutes
    LOG(INFO, "FastFlowLM") << "Waiting for model download to complete..." << std::endl;
    bool completed = false;
    int exit_code = -1;

#ifdef _WIN32
    DWORD wait_result = WaitForSingleObject(handle.handle, timeout_seconds * 1000);
    if (wait_result == WAIT_OBJECT_0) {
        DWORD win_exit_code;
        GetExitCodeProcess(handle.handle, &win_exit_code);
        exit_code = static_cast<int>(win_exit_code);
        completed = true;
    }
#else
    for (int i = 0; i < timeout_seconds * 10; ++i) {
        int status;
        pid_t result = waitpid(handle.pid, &status, WNOHANG);
        if (result > 0) {
            exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            completed = true;
            break;
        } else if (result < 0) {
            // Process doesn't exist or error
            completed = true;
            exit_code = -1;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Print progress every 5 seconds
        if (i % 50 == 0 && i > 0) {
            LOG(INFO, "FastFlowLM") << "Still downloading... (" << (i/10) << "s elapsed)" << std::endl;
        }
    }
#endif

    if (!completed) {
        utils::ProcessManager::stop_process(handle);
        throw std::runtime_error("FLM pull timed out after " + std::to_string(timeout_seconds) + " seconds");
    }

    if (exit_code != 0) {
        LOG(ERROR, "FastFlowLM") << "FLM pull failed with exit code: " << exit_code << std::endl;
        throw std::runtime_error("FLM pull failed with exit code: " + std::to_string(exit_code));
    }

    LOG(INFO, "FastFlowLM") << "Model pull completed successfully" << std::endl;
    return checkpoint;
}

void FastFlowLMServer::load(const std::string& model_name,
                           const ModelInfo& model_info,
                           const RecipeOptions& options,
                           bool do_not_upgrade) {
    LOG(INFO, "FastFlowLM") << "Loading model: " << model_name << std::endl;

    // Get FLM-specific options from RecipeOptions
    int ctx_size = options.get_option("ctx_size");
    std::string flm_args = options.get_option("flm_args");

    std::cout << "[FastFlowLM] Options";
    if (model_type_ == ModelType::LLM) {
        std::cout << ": context_target=" << ctx_size;
    }
    if (!flm_args.empty()) {
        std::cout << (model_type_ == ModelType::LLM ? ", " : ": ") << "flm_args=\"" << flm_args << "\"";
    }
    std::cout << std::endl;
    // Note: checkpoint_ is set by Router via set_model_metadata() before load() is called
    // We use checkpoint_ (base class field) for FLM API calls

#ifdef _WIN32
    // On Windows, auto-install FLM binary if needed (downloads zip and extracts)
    backend_manager_->install_backend(SPEC.recipe, "npu");
#endif

    // Validate NPU hardware/drivers
    std::string flm_path = get_flm_path();
    std::string validate_error;
    if (!utils::run_flm_validate(flm_path, validate_error)) {
        throw std::runtime_error("FLM NPU validation failed: " + validate_error +
            "\nVisit " + DRIVER_INSTALL_URL + " for driver installation instructions.");
    }

    // Download model if needed
    download_model(model_info.checkpoint(), do_not_upgrade);

    // Choose a port
    port_ = choose_port();

    long long runtime_ram_limit = -1;
    if (auto* cfg = RuntimeConfig::global()) {
        runtime_ram_limit = cfg->ram_limit();
    }

    ModelMemoryEstimate memory_estimate;
    int requested_ctx_size = ctx_size;
    int first_ctx_attempt = ctx_size;
    if (model_type_ == ModelType::LLM) {
        memory_estimate = MemoryManager::estimate_non_llamacpp_memory(
            model_info, MemoryBackendClass::NPU, requested_ctx_size, runtime_ram_limit);
        memory_estimate.kv_cache_bytes_per_token = estimate_flm_npu_context_guard_bytes_per_token(model_info);
        const auto initial_probe = MemoryManager::probe_system_memory(runtime_ram_limit);
        const uint64_t minimal_base_floor = saturating_add_local(memory_estimate.weight_bytes, 512ULL * 1024ULL * 1024ULL);
        if (memory_estimate.weight_bytes > 0 && minimal_base_floor > initial_probe.effective_available_bytes) {
            std::ostringstream oss;
            oss << "Insufficient memory to load base FLM/NPU model. Required at least "
                << MemoryManager::format_bytes(minimal_base_floor)
                << ", available/allowed "
                << MemoryManager::format_bytes(initial_probe.effective_available_bytes) << ".";
            throw std::runtime_error(oss.str());
        }
        if (memory_estimate.hard_error) {
            LOG(WARNING, "FastFlowLM")
                << "Base memory estimate exceeds available memory ("
                << memory_estimate.warning
                << "). Continuing with preflighted FLM start to avoid false negatives."
                << std::endl;
        }
        first_ctx_attempt = calculate_flm_npu_preflight_context(
            memory_estimate, requested_ctx_size, runtime_ram_limit);
        const auto probe = MemoryManager::probe_system_memory(runtime_ram_limit);
        LOG(INFO, "FastFlowLM") << "Context target=" << requested_ctx_size
                                 << ", preflight_ctx=" << first_ctx_attempt
                                 << ", available/allowed="
                                 << MemoryManager::format_bytes(probe.effective_available_bytes)
                                 << ", base_required="
                                 << MemoryManager::format_bytes(memory_estimate.base_required_bytes)
                                 << ", context_budget_per_token="
                                 << MemoryManager::format_bytes(memory_estimate.kv_cache_bytes_per_token)
                                 << std::endl;
        if (first_ctx_attempt < requested_ctx_size) {
            LOG(WARNING, "FastFlowLM")
                << "Context target preflight reduced NPU start context from "
                << requested_ctx_size << " to " << first_ctx_attempt
                << " to avoid a likely out-of-memory load."
                << std::endl;
        }
    }

    auto build_args = [&](int selected_ctx_size) {
        // Construct flm serve command based on model type.
        // Bind to localhost only for security.
        std::vector<std::string> args;
        if (model_type_ == ModelType::AUDIO) {
            // ASR mode: flm serve --asr 1
            args = {
                "serve",
                "--asr", "1",
                "--port", std::to_string(port_),
                "--host", "127.0.0.1",
                "--quiet"
            };
        } else if (model_type_ == ModelType::EMBEDDING) {
            // Embedding mode: flm serve --embed 1
            args = {
                "serve",
                "--embed", "1",
                "--port", std::to_string(port_),
                "--host", "127.0.0.1",
                "--quiet"
            };
        } else {
            // LLM mode (default): flm serve <checkpoint> --ctx-len N.
            // FLM does not expose a llama.cpp-style fit mode today, so Lemonade
            // preflights a conservative ctx-len before starting the process.
            args = {
                "serve",
                model_info.checkpoint(),
                "--ctx-len", std::to_string(selected_ctx_size),
                "--port", std::to_string(port_),
                "--host", "127.0.0.1",
                "--quiet"
            };
        }

        // Parse and append custom flm_args if provided.
        if (!flm_args.empty()) {
            std::istringstream iss(flm_args);
            std::string token;
            while (iss >> token) {
                args.push_back(token);
            }
        }
        return args;
    };

    auto start_with_ctx = [&](int selected_ctx_size) -> bool {
        std::vector<std::string> args = build_args(selected_ctx_size);
        LOG(INFO, "FastFlowLM") << "Starting flm-server"
                                 << (model_type_ == ModelType::LLM ? std::string(" with context target=") + std::to_string(selected_ctx_size) : std::string())
                                 << "..." << std::endl;
        LOG(INFO, "ProcessManager") << "Starting process: \"" << flm_path << "\"";
        for (const auto& arg : args) {
            LOG(INFO, "ProcessManager") << " \"" << arg << "\"";
        }
        LOG(INFO, "ProcessManager") << std::endl;

        process_handle_ = utils::ProcessManager::start_process(flm_path, args, "", is_debug(), true);
        LOG(INFO, "ProcessManager") << "Process started successfully" << std::endl;

        // Do not kill FLM while it is still starting based on transient
        // MemAvailable readings. The preflight above chooses a bounded first
        // ctx-len; after that, FLM process success/failure is the reliable
        // signal. This avoids false watchdog aborts and expensive retry loops.
        bool ready = wait_for_ready();
        if (!ready) {
            utils::ProcessManager::stop_process(process_handle_);
            process_handle_ = {nullptr, 0};
            return false;
        }


        return true;
    };

    int loaded_ctx_size = first_ctx_attempt;
    bool ready = false;
    if (model_type_ == ModelType::LLM) {
        std::vector<int> attempts;
        auto add_attempt = [&](int value) {
            if (value > 0 && std::find(attempts.begin(), attempts.end(), value) == attempts.end()) {
                attempts.push_back(value);
            }
        };
        add_attempt(first_ctx_attempt);
        if (first_ctx_attempt > 32768) add_attempt(32768);
        if (first_ctx_attempt > 8192) add_attempt(8192);
        if (first_ctx_attempt > MemoryManager::kProbeContext) add_attempt(MemoryManager::kProbeContext);

        for (int attempt_ctx : attempts) {
            loaded_ctx_size = attempt_ctx;
            ready = start_with_ctx(attempt_ctx);
            if (ready) break;
            LOG(WARNING, "FastFlowLM") << "Failed to start with context target=" << attempt_ctx
                                        << "; trying a smaller context if available" << std::endl;
        }
    } else {
        ready = start_with_ctx(ctx_size);
    }

    if (!ready) {
        throw std::runtime_error("flm-server failed to start");
    }

    is_loaded_ = true;
    if (model_type_ == ModelType::LLM) {
        memory_estimate.final_context = loaded_ctx_size;
        if (loaded_ctx_size < requested_ctx_size) {
            memory_estimate.restricted_context_warning = true;
            memory_estimate.warning = "Context target reduced due to FLM/NPU resource limits. Model loaded with "
                + std::to_string(loaded_ctx_size) + " context (target was " + std::to_string(requested_ctx_size) + ").";
            LOG(WARNING, "FastFlowLM") << memory_estimate.warning << std::endl;
        }
        set_memory_estimate(memory_estimate);
    }

    LOG(INFO, "FastFlowLM") << "Model loaded on port " << port_ << std::endl;
}

void FastFlowLMServer::unload() {
    LOG(INFO, "FastFlowLM") << "Unloading model..." << std::endl;
    if (is_loaded_ && process_handle_.pid != 0) {
        utils::ProcessManager::stop_process(process_handle_);
        process_handle_ = {nullptr, 0};
        port_ = 0;
        is_loaded_ = false;
    }
}

bool FastFlowLMServer::wait_for_ready() {
    // FLM doesn't have a health endpoint, so we use /api/tags to check if it's up
    std::string tags_url = get_base_url() + "/api/tags";

    LOG(INFO, "FastFlowLM") << "Waiting for " + server_name_ + " to be ready..." << std::endl;

    const int max_attempts = 300;  // 5 minutes timeout (large models can take time to load)
    for (int attempt = 0; attempt < max_attempts; ++attempt) {
        // Check if process is still running
        if (!utils::ProcessManager::is_running(process_handle_)) {
            LOG(ERROR, "FastFlowLM") << server_name_ << " process has terminated!" << std::endl;
            int exit_code = utils::ProcessManager::get_exit_code(process_handle_);
            LOG(ERROR, "FastFlowLM") << "Process exit code: " << exit_code << std::endl;
            LOG(ERROR, "FastFlowLM") << "Troubleshooting tips:" << std::endl;
            LOG(ERROR, "FastFlowLM") << "  1. Check if FLM is installed correctly: flm --version" << std::endl;
            LOG(ERROR, "FastFlowLM") << "  2. Try running: flm serve <model> --ctx-len 8192 --port 8001" << std::endl;
            LOG(ERROR, "FastFlowLM") << "  3. Check NPU drivers are installed (Windows only)" << std::endl;
            return false;
        }

        // Try to reach the /api/tags endpoint
        if (utils::HttpClient::is_reachable(tags_url, 1)) {
            LOG(INFO, "FastFlowLM") << server_name_ + " is ready!" << std::endl;
            return true;
        }

        // Sleep 1 second between attempts
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    LOG(ERROR, "FastFlowLM") << server_name_ << " failed to start within "
              << max_attempts << " seconds" << std::endl;
    return false;
}

json FastFlowLMServer::chat_completion(const json& request) {
    if (model_type_ == ModelType::AUDIO || model_type_ == ModelType::EMBEDDING) {
        return ErrorResponse::from_exception(
            UnsupportedOperationException("Chat completion", "FLM " + model_type_to_string(model_type_) + " model")
        );
    }

    // FLM requires the checkpoint name in the request (e.g., "gemma3:4b")
    // (whereas llama-server ignores the model name field)
    json modified_request = request;
    modified_request["model"] = checkpoint_;  // Use base class checkpoint field

    return forward_request("/v1/chat/completions", modified_request);
}

json FastFlowLMServer::completion(const json& request) {
    if (model_type_ == ModelType::AUDIO || model_type_ == ModelType::EMBEDDING) {
        return ErrorResponse::from_exception(
            UnsupportedOperationException("Text completion", "FLM " + model_type_to_string(model_type_) + " model")
        );
    }

    // FLM requires the checkpoint name in the request (e.g., "lfm2:1.2b")
    // (whereas llama-server ignores the model name field)
    json modified_request = request;
    modified_request["model"] = checkpoint_;  // Use base class checkpoint field

    return forward_request("/v1/completions", modified_request);
}

json FastFlowLMServer::embeddings(const json& request) {
    if (model_type_ == ModelType::AUDIO) {
        return ErrorResponse::from_exception(
            UnsupportedOperationException("Embeddings", "FLM " + model_type_to_string(model_type_) + " model")
        );
    }
    return forward_request("/v1/embeddings", request);
}

json FastFlowLMServer::reranking(const json& request) {
    if (model_type_ != ModelType::LLM) {
        return ErrorResponse::from_exception(
            UnsupportedOperationException("Reranking", "FLM " + model_type_to_string(model_type_) + " model")
        );
    }
    return forward_request("/v1/rerank", request);
}

json FastFlowLMServer::audio_transcriptions(const json& request) {
    if (model_type_ != ModelType::AUDIO) {
        return ErrorResponse::from_exception(
            UnsupportedOperationException("Audio transcription", "FLM " + model_type_to_string(model_type_) + " model")
        );
    }

    try {
        // Extract audio data from request (same format as WhisperServer)
        if (!request.contains("file_data")) {
            throw std::runtime_error("Missing 'file_data' in request");
        }

        std::string audio_data = request["file_data"].get<std::string>();
        std::string filename = request.value("filename", "audio.wav");

        // Determine content type from filename extension
        std::filesystem::path filepath(filename);
        std::string ext = filepath.extension().string();
        std::string content_type = "audio/wav";
        if (ext == ".mp3") content_type = "audio/mpeg";
        else if (ext == ".m4a") content_type = "audio/mp4";
        else if (ext == ".ogg") content_type = "audio/ogg";
        else if (ext == ".flac") content_type = "audio/flac";
        else if (ext == ".webm") content_type = "audio/webm";

        // Build multipart fields for FLM's /v1/audio/transcriptions endpoint
        std::vector<utils::MultipartField> fields;

        // Audio file field
        fields.push_back({
            "file",
            audio_data,
            filepath.filename().string(),
            content_type
        });

        // Model field (required by OpenAI API format)
        fields.push_back({"model", checkpoint_, "", ""});

        // Optional parameters
        if (request.contains("language")) {
            fields.push_back({"language", request["language"].get<std::string>(), "", ""});
        }
        if (request.contains("prompt")) {
            fields.push_back({"prompt", request["prompt"].get<std::string>(), "", ""});
        }
        if (request.contains("response_format")) {
            fields.push_back({"response_format", request["response_format"].get<std::string>(), "", ""});
        }
        if (request.contains("temperature")) {
            fields.push_back({"temperature", std::to_string(request["temperature"].get<double>()), "", ""});
        }

        return forward_multipart_request("/v1/audio/transcriptions", fields);

    } catch (const std::exception& e) {
        return json{
            {"error", {
                {"message", std::string("Transcription failed: ") + e.what()},
                {"type", "audio_processing_error"}
            }}
        };
    }
}

json FastFlowLMServer::responses(const json& request) {
    // Responses API is not supported for FLM backend
    return ErrorResponse::from_exception(
        UnsupportedOperationException("Responses API", "flm")
    );
}

void FastFlowLMServer::forward_streaming_request(const std::string& endpoint,
                                                  const std::string& request_body,
                                                  httplib::DataSink& sink,
                                                  bool sse,
                                                  long timeout_seconds) {
    // Streaming is only supported for LLM models
    if (model_type_ == ModelType::AUDIO || model_type_ == ModelType::EMBEDDING) {
        std::string error_msg = "data: {\"error\":{\"message\":\"Streaming not supported for FLM "
            + model_type_to_string(model_type_) + " model\",\"type\":\"unsupported_operation\"}}\n\n";
        sink.write(error_msg.c_str(), error_msg.size());
        return;
    }

    // FLM requires the checkpoint name in the model field (e.g., "gemma3:4b"),
    // not the Lemonade model name (e.g., "Gemma3-4b-it-FLM")
    try {
        json request = json::parse(request_body);
        request["model"] = checkpoint_;  // Use base class checkpoint field
        std::string modified_body = request.dump();

        // Call base class with modified request
        WrappedServer::forward_streaming_request(endpoint, modified_body, sink, sse, timeout_seconds);
    } catch (const json::exception& e) {
        // If JSON parsing fails, forward original request
        WrappedServer::forward_streaming_request(endpoint, request_body, sink, sse, timeout_seconds);
    }
}

std::string FastFlowLMServer::get_flm_path() {
#ifdef _WIN32
    // On Windows, use the standard install directory (auto-installed zip)
    try {
        std::string path = BackendUtils::get_backend_binary_path(SPEC, "npu");
        LOG(INFO, "FastFlowLM") << "Found flm at: " << path << std::endl;
        return path;
    } catch (const std::exception& e) {
        LOG(ERROR, "FastFlowLM") << "flm not found in install dir: " << e.what() << std::endl;
        return "";
    }
#else
    // On Linux, FLM is installed as a system package (in PATH)
    std::string flm_path = utils::find_flm_executable();
    if (!flm_path.empty()) {
        LOG(INFO, "FastFlowLM") << "Found flm at: " << flm_path << std::endl;
    } else {
        LOG(ERROR, "FastFlowLM") << "flm not found in PATH" << std::endl;
    }
    return flm_path;
#endif
}

} // namespace backends
} // namespace lemon
