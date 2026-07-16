#include "lemon/backends/fastflowlm/fastflowlm_server.h"
#include "lemon/backends/fastflowlm/fastflowlm.h"
#include "lemon/backends/fastflowlm/fastflowlm_models.h"
#include "lemon/backends/fastflowlm/flm_arg_resolver.h"
#include "lemon/backends/backend_registry.h"
#include "lemon/backends/backend_ops.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/model_manager.h"
#include "lemon/system_info.h"
#include "lemon/error_types.h"
#include "lemon/utils/process_manager.h"
#include "lemon/utils/http_client.h"
#include "lemon/utils/path_utils.h"
#include "lemon/utils/json_utils.h"
#include <iostream>
#include <filesystem>
#include <cstdlib>
#include <optional>
#include <thread>
#include <chrono>
#include <fstream>
#include <algorithm>
#include <lemon/utils/aixlog.hpp>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/wait.h>
#endif

namespace fs = std::filesystem;

namespace lemon {
namespace backends {

static const std::string DRIVER_INSTALL_URL = "https://lemonade-server.ai/driver_install.html";


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
    params.filename = "fastflowlm_" + bare_version + "_linux.tar.gz";
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

    auto handle = utils::ProcessManager::start_process(flm_path, args, "", is_debug());

    // Wait for process to complete (handles both fast exits and long downloads).
    // Use the owning wait primitive directly because this helper intentionally
    // reaps the short-lived `flm pull` child.
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

    int ctx_size = options.get_option("ctx_size");
    std::string flm_args = options.get_option("flm_args");
    FLMArgResolution flm_arg_resolution = resolve_flm_args(flm_args, ctx_size);

    std::cout << "[FastFlowLM] Options: ctx_size=" << ctx_size << std::endl;
    // Note: checkpoint_ is set by Router via set_model_metadata() before load() is called
    // We use checkpoint_ (base class field) for FLM API calls

    if (fastflowlm::find_flm_in_path().empty()) {
        try {
            backend_manager_->install_backend(fastflowlm::spec()->recipe, "npu");
        } catch (const std::exception&) {
            if (fastflowlm::find_flm_executable().empty()) {
                throw;
            }
            LOG(DEBUG, "FastFlowLM") << "Using system FLM from PATH" << std::endl;
        }
    }

    std::string flm_path = get_flm_path();
    std::string validate_error;
    if (!fastflowlm::run_flm_validate(flm_path, validate_error)) {
        throw std::runtime_error("FLM NPU validation failed: " + validate_error +
            "\nVisit " + DRIVER_INSTALL_URL + " for driver installation instructions.");
    }

    download_model(model_info.checkpoint(), do_not_upgrade);

    port_ = choose_port();

    // Bind to localhost only for security
    std::vector<std::string> args;
    if (model_type_ == ModelType::TRANSCRIPTION) {
        args = {
            "serve",
            "--asr", "1",
            "--port", std::to_string(port_),
            "--host", "127.0.0.1"
        };
    } else if (model_type_ == ModelType::EMBEDDING) {
        args = {
            "serve",
            "--embed", "1",
            "--port", std::to_string(port_),
            "--host", "127.0.0.1"
        };
    } else {
        args = {
            "serve",
            model_info.checkpoint(),
            "--ctx-len", std::to_string(ctx_size),
            "--port", std::to_string(port_),
            "--host", "127.0.0.1"
        };
    }

    args.insert(args.end(), flm_arg_resolution.args.begin(), flm_arg_resolution.args.end());
    args.push_back("--quiet");

    LOG(INFO, "FastFlowLM") << "Starting flm-server..." << std::endl;
    LOG(INFO, "ProcessManager") << "Starting process: \"" << flm_path << "\"";
    for (const auto& arg : args) {
        LOG(INFO, "ProcessManager") << " \"" << arg << "\"";
    }
    LOG(INFO, "ProcessManager") << std::endl;

    set_process_handle(utils::ProcessManager::start_process(flm_path, args, "", is_debug(), true));
    LOG(INFO, "ProcessManager") << "Process started successfully" << std::endl;

    bool ready = wait_for_ready();
    if (!ready) {
        const ProcessHandle handle = consume_process_handle_for_cleanup();
        if (has_process_handle(handle)) {
            utils::ProcessManager::stop_process(handle);
        }
        throw std::runtime_error("flm-server failed to start");
    }

    is_loaded_ = true;
    LOG(INFO, "FastFlowLM") << "Model loaded on port " << get_backend_port() << std::endl;
}

void FastFlowLMServer::unload() {
    stop_backend_watchdog();
    LOG(INFO, "FastFlowLM") << "Unloading model..." << std::endl;

    const ProcessHandle handle = consume_process_handle_for_cleanup();
    if (has_process_handle(handle)) {
        utils::ProcessManager::stop_process(handle);
    }
    is_loaded_ = false;
}

bool FastFlowLMServer::wait_for_ready() {
    // FLM doesn't have a health endpoint, so we use /api/tags to check if it's up
    std::string tags_url = get_base_url() + "/api/tags";

    LOG(INFO, "FastFlowLM") << "Waiting for " + server_name_ + " to be ready..." << std::endl;

    const int max_attempts = 300;  // 5 minutes timeout (large models can take time to load)
    for (int attempt = 0; attempt < max_attempts; ++attempt) {
        // Check if process is still running. If it already exited, consume and
        // reap the owned handle here so failed-start cleanup cannot later signal
        // a stale PID.
        const ProcessHandle handle = get_process_handle_snapshot();
        if (!has_process_handle(handle) || !utils::ProcessManager::is_running(handle)) {
            LOG(ERROR, "FastFlowLM") << server_name_ << " process has terminated!" << std::endl;
            const ProcessHandle exited_handle = consume_process_handle_for_cleanup();
            int exit_code = has_process_handle(exited_handle)
                ? utils::ProcessManager::reap_process(exited_handle)
                : -1;
            LOG(ERROR, "FastFlowLM") << "Process exit code: " << exit_code << std::endl;
            LOG(ERROR, "FastFlowLM") << "Troubleshooting tips:" << std::endl;
            LOG(ERROR, "FastFlowLM") << "  1. Check if FLM is installed correctly: flm --version" << std::endl;
            LOG(ERROR, "FastFlowLM") << "  2. Try running: flm serve <model> --ctx-len 8192 --port 8001" << std::endl;
            LOG(ERROR, "FastFlowLM") << "  3. Check NPU drivers are installed (Windows only)" << std::endl;
            return false;
        }

        if (utils::HttpClient::is_reachable(tags_url, 1)) {
            LOG(INFO, "FastFlowLM") << server_name_ + " is ready!" << std::endl;
            start_backend_watchdog("/api/tags");
            return true;
        }

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    LOG(ERROR, "FastFlowLM") << server_name_ << " failed to start within "
              << max_attempts << " seconds" << std::endl;
    return false;
}

json FastFlowLMServer::chat_completion(const json& request) {
    if (model_type_ == ModelType::TRANSCRIPTION || model_type_ == ModelType::EMBEDDING) {
        return ErrorResponse::from_exception(
            UnsupportedOperationException("Chat completion", "FLM " + model_type_to_string(model_type_) + " model")
        );
    }

    // FLM requires the checkpoint name in the request (e.g., "gemma3:4b")
    // (whereas llama-server ignores the model name field)
    json modified_request = request;
    modified_request["model"] = checkpoint_;

    return forward_request("/v1/chat/completions", modified_request);
}

json FastFlowLMServer::completion(const json& request) {
    if (model_type_ == ModelType::TRANSCRIPTION || model_type_ == ModelType::EMBEDDING) {
        return ErrorResponse::from_exception(
            UnsupportedOperationException("Text completion", "FLM " + model_type_to_string(model_type_) + " model")
        );
    }

    // FLM requires the checkpoint name in the request (e.g., "lfm2:1.2b")
    // (whereas llama-server ignores the model name field)
    json modified_request = request;
    modified_request["model"] = checkpoint_;

    return forward_request("/v1/completions", modified_request);
}

json FastFlowLMServer::embeddings(const json& request) {
    if (model_type_ == ModelType::TRANSCRIPTION) {
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
    if (model_type_ != ModelType::TRANSCRIPTION) {
        return ErrorResponse::from_exception(
            UnsupportedOperationException("Audio transcription", "FLM " + model_type_to_string(model_type_) + " model")
        );
    }

    try {
        if (!request.contains("file_data")) {
            throw std::runtime_error("Missing 'file_data' in request");
        }

        std::string audio_data = request["file_data"].get<std::string>();
        std::string filename = request.value("filename", "audio.wav");

        std::filesystem::path filepath(filename);
        std::string ext = filepath.extension().string();
        std::string content_type = "audio/wav";
        if (ext == ".mp3") content_type = "audio/mpeg";
        else if (ext == ".m4a") content_type = "audio/mp4";
        else if (ext == ".ogg") content_type = "audio/ogg";
        else if (ext == ".flac") content_type = "audio/flac";
        else if (ext == ".webm") content_type = "audio/webm";

        std::vector<utils::MultipartField> fields;

        fields.push_back({
            "file",
            audio_data,
            filepath.filename().string(),
            content_type
        });

        // Model field (required by OpenAI API format)
        fields.push_back({"model", checkpoint_, "", ""});

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
    return ErrorResponse::from_exception(
        UnsupportedOperationException("Responses API", "flm")
    );
}

void FastFlowLMServer::forward_streaming_request(const std::string& endpoint,
                                                  const std::string& request_body,
                                                  httplib::DataSink& sink,
                                                  bool sse,
                                                  long timeout_seconds,
                                                  TelemetryCallback telemetry_callback) {
    if (model_type_ == ModelType::TRANSCRIPTION || model_type_ == ModelType::EMBEDDING) {
        std::string error_msg = "data: {\"error\":{\"message\":\"Streaming not supported for FLM "
            + model_type_to_string(model_type_) + " model\",\"type\":\"unsupported_operation\"}}\n\n";
        sink.write(error_msg.c_str(), error_msg.size());
        sink.done();
        return;
    }

    // FLM requires the checkpoint name in the model field (e.g., "gemma3:4b"),
    // not the Lemonade model name (e.g., "Gemma3-4b-it-FLM")
    try {
        json request = json::parse(request_body);
        request["model"] = checkpoint_;
        std::string modified_body = request.dump();

        WrappedServer::forward_streaming_request(endpoint, modified_body, sink, sse,
                                                 timeout_seconds, telemetry_callback);
    } catch (const json::exception& e) {
        // If JSON parsing fails, forward original request
        WrappedServer::forward_streaming_request(endpoint, request_body, sink, sse,
                                                 timeout_seconds, telemetry_callback);
    }
}

std::string FastFlowLMServer::get_flm_path() {
    std::string flm_path = fastflowlm::find_flm_executable();
    if (!flm_path.empty()) {
        LOG(INFO, "FastFlowLM") << "Found flm at: " << flm_path << std::endl;
    } else {
        LOG(ERROR, "FastFlowLM") << "flm not found in PATH or install dir" << std::endl;
    }
    return flm_path;
}

} // namespace backends
} // namespace lemon

namespace lemon {
namespace backends {
namespace fastflowlm {

std::unique_ptr<WrappedServer> create(const BackendContext& ctx) {
    return make_server<FastFlowLMServer>(ctx);
}

namespace {
// FLM model-management behavior: max context window from the model's config.json.
class FlmOps : public BackendOps {
public:
    void populate_metadata(ModelInfo& info, const BackendOpsContext&) const override {
        info.max_context_window = read_flm_max_context_window(info);
    }

    std::string resolve_checkpoint_path(const ModelInfo&,
                                        const CheckpointResolveContext& ctx) const override {
        // FLM uses the checkpoint string as-is (e.g. "gemma3:4b"); no local file.
        return ctx.checkpoint;
    }

    std::vector<ModelInfo> discover_models(const BackendOpsContext&) const override {
        return flm_discover_models();
    }

    bool is_downloaded(const ModelInfo& info, const BackendOpsContext&) const override {
        const auto installed = flm_installed_checkpoints();
        return std::find(installed.begin(), installed.end(), info.checkpoint()) != installed.end();
    }

    void download_model(const ModelInfo& info, bool do_not_upgrade, DownloadProgressCallback progress,
                        const BackendOpsContext&) const override {
        flm_download(info.checkpoint(), do_not_upgrade, progress);
    }

    bool invalidates_cache_after_download() const override { return true; }

    std::string resolve_version(const std::string&, const std::string& file_version) const override {
        // On Linux FLM is a system package with no version.txt; query the CLI.
        if (file_version.empty() || file_version == "unknown") {
            return flm_version();
        }
        return file_version;
    }

    InstallCheck check_install(const std::string&, bool binary_found) const override {
        return {!find_flm_executable().empty(), ""};
    }

};
}  // namespace

const BackendSpec* spec() { return make_spec<FastFlowLMServer>(descriptor); }
const BackendOps* ops() { return single_ops<FlmOps>(); }
}  // namespace fastflowlm
}  // namespace backends
}  // namespace lemon
