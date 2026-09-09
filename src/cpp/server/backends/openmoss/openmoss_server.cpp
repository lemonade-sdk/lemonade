#include "lemon/backends/openmoss/openmoss_server.h"
#include "lemon/backends/openmoss/openmoss.h"
#include "lemon/backends/backend_registry.h"
#include "lemon/backends/backend_ops.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backend_manager.h"
#include "lemon/error_types.h"
#include "lemon/model_manager.h"
#include "lemon/runtime_config.h"
#include "lemon/system_info.h"
#include "lemon/utils/http_client.h"
#include "lemon/utils/json_utils.h"
#include "lemon/utils/process_manager.h"
#include <lemon/utils/aixlog.hpp>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <thread>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace lemon {
namespace backends {

namespace {
constexpr const char* kVoiceDesignPhrase =
    "Hello there. This is a short sample of the voice you described.";

constexpr const char* kVoiceDesignField = "voice_design_description";
constexpr long kVoiceDesignDeadlineSeconds = 300;
constexpr long kVoiceDesignRecoverySeconds = 60;

class ProcessSwapGuard {

public:

    explicit ProcessSwapGuard(std::atomic<bool>& flag) : flag_(flag) {
        flag_.store(true, std::memory_order_release);
    }

    ~ProcessSwapGuard() {
        flag_.store(false, std::memory_order_release);
    }

    ProcessSwapGuard(const ProcessSwapGuard&) = delete;
    ProcessSwapGuard& operator=(const ProcessSwapGuard&) = delete;

private:

    std::atomic<bool>& flag_;

};

std::string string_field(const json& request, const char* key) {
    const auto it = request.find(key);
    return (it != request.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

bool client_cancelled(const httplib::DataSink& sink) {
    return sink.is_writable && !sink.is_writable();
}

long remaining_seconds(std::chrono::steady_clock::time_point deadline) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) return 0;
    const auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - now).count();
    return std::max<long>(1, (remaining_ms + 999) / 1000);
}
}  // namespace

InstallParams OpenMossServer::get_install_params(const std::string& backend, const std::string& version) {
    (void)version;
    InstallParams params;
    params.repo = "pwilkin/openmoss";
    const std::string variant = (backend.rfind("rocm", 0) == 0) ? "rocm" : backend;
#ifdef _WIN32
    params.filename = "moss-tts-" + variant + "-windows-x64.zip";
#else
    params.filename = "moss-tts-" + variant + "-linux-x64.tar.gz";
#endif
    return params;
}

OpenMossServer::OpenMossServer(const std::string& log_level,
                               ModelManager* model_manager,
                               BackendManager* backend_manager)
    : WrappedServer("openmoss-server", log_level, model_manager, backend_manager) {}

OpenMossServer::~OpenMossServer() {
    unload();
}


bool OpenMossServer::is_backend_alive() const {
    return process_swap_in_progress_.load(std::memory_order_acquire)
        || WrappedServer::is_backend_alive();
}

AudioFormatMetadata OpenMossServer::audio_format_metadata(
    const std::string& response_format) const {
    if (response_format != "pcm") {
        return {};
    }
    AudioFormatMetadata metadata;
    metadata.content_type = "audio/pcm";
    const int sample_rate = pcm_sample_rate_.load(std::memory_order_acquire);
    const int channels = pcm_channels_.load(std::memory_order_acquire);
    if (sample_rate > 0) {
        metadata.headers["X-MOSS-Sample-Rate"] = std::to_string(sample_rate);
    }
    if (channels > 0) {
        metadata.headers["X-MOSS-Channels"] = std::to_string(channels);
    }
    return metadata;
}

std::string OpenMossServer::resolve_binary_path(const std::string& backend) {
    const BackendSpec* spec = openmoss::spec();
    std::string external = BackendUtils::find_external_backend_binary(spec->recipe, backend);
    if (!external.empty() && std::filesystem::exists(external)) {
        return external;
    }
    backend_manager_->install_backend(spec->recipe, backend);
    return BackendUtils::get_backend_binary_path(*spec, backend);
}

void OpenMossServer::load(const std::string& model_name,
                          const ModelInfo& model_info,
                          const RecipeOptions& options,
                          bool /*do_not_upgrade*/) {
    LOG(INFO, "openmoss-server") << "Loading model: " << model_name << std::endl;

    const std::string model_path = model_info.resolved_path();
    if (model_path.empty() || !std::filesystem::exists(model_path)) {
        throw std::runtime_error("Model path not found for checkpoint: " + model_info.checkpoint());
    }

    std::string backend = options.get_option("openmoss_backend");
    if (backend.empty()) {
        auto supported = SystemInfo::get_supported_backends("openmoss");
        if (supported.backends.empty()) {
            throw UnsupportedOperationException(
                "OpenMOSS TTS", "this system: no supported GPU backend (Vulkan, ROCm, or CUDA) detected");
        }
        backend = supported.backends[0];
    }
    RuntimeConfig::validate_backend_choice("openmoss", backend);
    const std::string exe_path = resolve_binary_path(backend);

    std::vector<std::pair<std::string, std::string>> env_vars;
    const std::string exe_dir = std::filesystem::path(exe_path).parent_path().string();
    auto prepend_loader_path = [&env_vars, &exe_dir](const std::string& extra_dirs) {
#ifdef _WIN32
        std::string path = extra_dirs.empty() ? exe_dir : (extra_dirs + ";" + exe_dir);
        if (const char* p = std::getenv("PATH")) path += std::string(";") + p;
        env_vars.push_back({"PATH", path});
#else
        std::string ld = extra_dirs.empty() ? exe_dir : (extra_dirs + ":" + exe_dir);
        if (const char* p = std::getenv("LD_LIBRARY_PATH")) ld += std::string(":") + p;
        env_vars.push_back({"LD_LIBRARY_PATH", ld});
#endif
    };
    if (backend == "rocm") {
        const std::string arch = SystemInfo::get_rocm_arch();
        std::string dirs;
        if (!arch.empty()) {
            dirs = BackendUtils::join_runtime_dirs(
                BackendUtils::get_therock_lib_paths(arch));
        }
        prepend_loader_path(dirs);
    } else if (backend == "cuda") {
        prepend_loader_path("");
        BackendUtils::apply_cuda_env_vars(env_vars, "openmoss-server");
    }

    std::unique_lock<std::shared_mutex> lock(request_mutex_);
    exe_path_ = exe_path;
    env_vars_ = env_vars;
    model_path_ = model_path;
    voicegen_path_ = model_info.resolved_path("voicegen");
    if (!voicegen_path_.empty() && !std::filesystem::exists(voicegen_path_)) {
        voicegen_path_.clear();
    }
    const json audio_defaults =
        model_info.extra<json>("audio_defaults", json::object());
    pcm_sample_rate_.store(
        utils::JsonUtils::get_or_default<int>(
            audio_defaults, "pcm_sample_rate", 0),
        std::memory_order_release);
    pcm_channels_.store(
        utils::JsonUtils::get_or_default<int>(
            audio_defaults, "pcm_channels", 0),
        std::memory_order_release);
    reference_cache_.clear();

    start_speech_process();
}

OpenMossServer::Subprocess OpenMossServer::spawn(const std::string& model_path) {
    Subprocess proc;
    proc.port = utils::ProcessManager::find_free_port(8001);
    if (proc.port <= 0) {
        throw std::runtime_error("Failed to find an available port");
    }

    proc.args = {
        "--model", model_path,
        "--host", "127.0.0.1",
        "--port", std::to_string(proc.port),
    };
    proc.args.push_back("--no-webui");

    LOG(INFO, "openmoss-server") << "Starting " << exe_path_ << " on port " << proc.port << std::endl;
    proc.handle = utils::ProcessManager::start_process(
        exe_path_, proc.args, "", is_debug(), false, env_vars_);
    if (!has_process_handle(proc.handle)) {
        throw std::runtime_error("Failed to start openmoss-server process");
    }
    return proc;
}

void OpenMossServer::stop_speech_process() {
    stop_backend_watchdog();
    const ProcessHandle handle = consume_process_handle_for_cleanup();
    if (has_process_handle(handle)) {
        LOG(INFO, "openmoss-server") << "Stopping server (PID: " << handle.pid << ")" << std::endl;
        utils::ProcessManager::stop_process(handle);
    }
}

void OpenMossServer::start_speech_process(long timeout_seconds) {
    Subprocess proc = spawn(model_path_);
    set_process_state(proc.handle, proc.port, exe_path_, proc.args);
    LOG(INFO, "openmoss-server") << "Process started with PID: " << proc.handle.pid << std::endl;

    if (!wait_for_ready("/health", timeout_seconds)) {
        stop_speech_process();
        throw std::runtime_error("openmoss-server failed to start or become ready");
    }
}

void OpenMossServer::unload() {
    std::unique_lock<std::shared_mutex> lock(request_mutex_);
    stop_speech_process();
    reference_cache_.clear();
}

std::string OpenMossServer::design_reference_sample(
    const std::string& voice_description, httplib::DataSink& sink) {
    auto cached = reference_cache_.find(voice_description);
    if (cached != reference_cache_.end()) {
        return cached->second;
    }

    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(kVoiceDesignDeadlineSeconds);
    ProcessSwapGuard swap_guard(process_swap_in_progress_);
    LOG(INFO, "openmoss-server") << "Designing reference voice for: " << voice_description << std::endl;
    stop_speech_process();

    std::string sample;
    try {
        sample = render_reference_sample(voice_description, sink, deadline);
        const long restart_timeout = remaining_seconds(deadline);
        if (restart_timeout <= 0) {
            throw std::runtime_error("voice design deadline expired before speech restart");
        }
        start_speech_process(restart_timeout);
    } catch (...) {
        try {
            start_speech_process(kVoiceDesignRecoverySeconds);
        } catch (const std::exception& e) {
            LOG(ERROR, "openmoss-server")
                << "Speech model failed to restart after an unsuccessful voice design: "
                << e.what() << std::endl;
        }
        throw;
    }

    reference_cache_[voice_description] = sample;
    return sample;
}

std::string OpenMossServer::render_reference_sample(
    const std::string& voice_description, httplib::DataSink& sink,
    std::chrono::steady_clock::time_point deadline) {
    Subprocess designer = spawn(voicegen_path_);
    std::string sample;
    try {
        const std::string base = "http://127.0.0.1:" + std::to_string(designer.port);
        bool ready = false;
        while (!ready && std::chrono::steady_clock::now() < deadline) {
            if (client_cancelled(sink)) {
                throw std::runtime_error("voice design cancelled by client");
            }
            if (!utils::ProcessManager::is_running(designer.handle)) {
                const int exit_code = utils::ProcessManager::reap_process(designer.handle);
                designer.handle = ProcessHandle{};
                throw std::runtime_error(
                    "voice-design backend exited during startup with code "
                    + std::to_string(exit_code));
            }
            try {
                const long health_timeout = std::min<long>(2, remaining_seconds(deadline));
                if (health_timeout <= 0) break;
                auto health = utils::HttpClient::get(
                    base + "/health", {}, health_timeout,
                    utils::HttpSecurityPolicy::TrustedLoopback);
                ready = (health.status_code == 200);
            } catch (const std::exception&) {
                ready = false;
            }
            if (!ready && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
        }
        if (!ready) {
            throw std::runtime_error("voice-design backend timed out while becoming ready");
        }
        if (client_cancelled(sink)) {
            throw std::runtime_error("voice design cancelled by client");
        }

        const long post_timeout = remaining_seconds(deadline);
        if (post_timeout <= 0) {
            throw std::runtime_error("voice design deadline expired before generation");
        }

        json body;
        body["input"] = kVoiceDesignPhrase;
        body["voice"] = voice_description;
        body["response_format"] = "wav";

        int backend_status = 200;
        std::string response_body;
        auto response = utils::HttpClient::post_stream(
            base + "/v1/audio/speech",
            body.dump(),
            [&response_body](const char* data, size_t length) {
                response_body.append(data, length);
                return true;
            },
            {{"Content-Type", "application/json"}},
            post_timeout,
            [&backend_status](int status) { backend_status = status; },
            utils::HttpSecurityPolicy::TrustedLoopback,
            [&sink]() { return client_cancelled(sink); });

        if (client_cancelled(sink)) {
            throw std::runtime_error("voice design cancelled by client");
        }
        if (response.curl_code != 0) {
            throw std::runtime_error(
                "voice design transport failed: " + response.curl_error);
        }
        const int status = backend_status != 200 ? backend_status : response.status_code;
        if (status != 200 || response_body.empty()) {
            throw std::runtime_error("voice design failed: HTTP " + std::to_string(status));
        }
        sample = utils::JsonUtils::base64_encode(response_body);
    } catch (...) {
        if (has_process_handle(designer.handle)) {
            utils::ProcessManager::stop_process(designer.handle);
        }
        throw;
    }
    utils::ProcessManager::stop_process(designer.handle);
    LOG(INFO, "openmoss-server") << "Voice-design subprocess released" << std::endl;
    return sample;
}

json OpenMossServer::apply_voice_design(const json& request, httplib::DataSink& sink) {
    json forwarded = request;

    const std::string description = string_field(forwarded, kVoiceDesignField);
    forwarded.erase(kVoiceDesignField);
    if (description.empty() || forwarded.contains("reference_wav_b64")) {
        return forwarded;
    }
    if (voicegen_path_.empty()) {
        throw std::runtime_error(
            "This model has no voice-design component; attach reference audio instead.");
    }
    forwarded["reference_wav_b64"] = design_reference_sample(description, sink);
    return forwarded;
}

void OpenMossServer::audio_speech(const json& request, httplib::DataSink& sink) {
    const std::string description = string_field(request, kVoiceDesignField);
    const bool needs_voice_design = !description.empty() && !request.contains("reference_wav_b64");
    auto forward = [&]() {
        json forwarded = apply_voice_design(request, sink);
        if (forwarded.contains("stream_format")) {
            forwarded["stream"] = true;
        }


        forward_streaming_request(
            "/v1/audio/speech", forwarded.dump(), sink, /*sse=*/false, /*timeout_seconds=*/600);
    };

    if (needs_voice_design) {
        std::unique_lock<std::shared_mutex> lock(request_mutex_);
        forward();
        return;
    }

    std::shared_lock<std::shared_mutex> lock(request_mutex_);
    forward();
}

void OpenMossServer::audio_generations(const json& request, httplib::DataSink& sink) {
    std::shared_lock<std::shared_mutex> lock(request_mutex_);
    json body;
    body["prompt"] = string_field(request, "prompt");
    for (const char* key : {"seconds", "steps", "cfg_scale", "sigma_shift",
                            "negative_prompt", "seed", "append_duration_suffix",
                            "response_format"}) {
        if (request.contains(key)) body[key] = request[key];
    }
    if (!body.contains("seconds") && request.contains("duration")) {
        body["seconds"] = request["duration"];
    }
    if (!body.contains("cfg_scale") && request.contains("cfg")) {
        body["cfg_scale"] = request["cfg"];
    }
    forward_streaming_request("/sfx", body.dump(), sink, /*sse=*/false, /*timeout_seconds=*/900);
}

}  // namespace backends

namespace backends {

namespace {
class OpenMossOps : public BackendOps {
public:
    std::optional<std::vector<std::string>> select_checkpoint_files(
        const std::string& main_variant, const std::vector<std::string>& repo_files) const override {
        std::vector<std::string> want = {main_variant};
        auto pos = main_variant.rfind(".gguf");
        if (pos != std::string::npos) {
            std::string extras = main_variant.substr(0, pos) + ".extras.gguf";
            for (const auto& f : repo_files) {
                if (f == extras) { want.push_back(extras); break; }
            }
        }
        return want;
    }
};
}  // namespace

namespace openmoss {

std::unique_ptr<WrappedServer> create(const BackendContext& ctx) {
    return make_server<OpenMossServer>(ctx);
}

const BackendSpec* spec() { return make_spec<OpenMossServer>(descriptor); }
const BackendOps* ops() { return single_ops<OpenMossOps>(); }

}  // namespace openmoss
}  // namespace backends
}  // namespace lemon
