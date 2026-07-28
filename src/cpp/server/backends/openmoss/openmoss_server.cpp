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
// The utterance the voice-design model renders; its content is irrelevant, only
// the timbre it is rendered in, which becomes the reference for the speech model.
constexpr const char* kVoiceDesignPhrase =
    "Hello there. This is a short sample of the voice you described.";
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
        const std::string therock_lib = arch.empty() ? "" : BackendUtils::get_therock_lib_path(arch);
        std::string dirs;
        if (!therock_lib.empty()) {
#ifdef _WIN32
            const std::string llvm_bin =
                (std::filesystem::path(therock_lib).parent_path() / "lib" / "llvm" / "bin").string();
            dirs = therock_lib + ";" + llvm_bin;
#else
            dirs = therock_lib + ":" + therock_lib + "/llvm/lib";
#endif
        }
        prepend_loader_path(dirs);
    } else if (backend == "cuda") {
        prepend_loader_path("");
        BackendUtils::apply_cuda_env_vars(env_vars, "openmoss-server");
    }

    exe_path_ = exe_path;
    env_vars_ = env_vars;
    voicegen_path_ = model_info.resolved_path("voicegen");
    if (!voicegen_path_.empty() && !std::filesystem::exists(voicegen_path_)) {
        voicegen_path_.clear();
    }
    reference_cache_.clear();

    Subprocess main_proc = spawn(model_path);
    port_ = main_proc.port;
    set_process_handle(main_proc.handle);
    LOG(INFO, "openmoss-server") << "Process started with PID: " << main_proc.handle.pid << std::endl;

    if (!wait_for_ready("/health")) {
        unload();
        throw std::runtime_error("openmoss-server failed to start or become ready");
    }
}

OpenMossServer::Subprocess OpenMossServer::spawn(const std::string& model_path) {
    Subprocess proc;
    // Deliberately not choose_port(): that assigns port_, which addresses the
    // resident speech process. A transient voice-design child must not retarget it.
    proc.port = utils::ProcessManager::find_free_port(8001);
    if (proc.port <= 0) {
        throw std::runtime_error("Failed to find an available port");
    }

    // A reference-conditioned request prefills the whole sample as audio tokens
    // (~12.5 frames/s x n_vq codebooks), which overruns the server's 8192/512
    // defaults for anything but a very short clip.
    const std::vector<std::string> args = {
        "--model", model_path,
        "--host", "127.0.0.1",
        "--port", std::to_string(proc.port),
        "--n-ctx", "32768",
        "--n-batch", "4096",
        "--no-webui",
    };

    LOG(INFO, "openmoss-server") << "Starting " << exe_path_ << " on port " << proc.port << std::endl;
    proc.handle = utils::ProcessManager::start_process(
        exe_path_, args, "", is_debug(), false, env_vars_);
    if (!has_process_handle(proc.handle)) {
        throw std::runtime_error("Failed to start openmoss-server process");
    }
    return proc;
}

void OpenMossServer::unload() {
    stop_backend_watchdog();
    const ProcessHandle handle = consume_process_handle_for_cleanup();
    if (has_process_handle(handle)) {
        LOG(INFO, "openmoss-server") << "Stopping server (PID: " << handle.pid << ")" << std::endl;
        utils::ProcessManager::stop_process(handle);
    }
    reference_cache_.clear();
}

std::string OpenMossServer::design_reference_sample(const std::string& voice_description) {
    std::lock_guard<std::mutex> lock(design_mutex_);
    auto cached = reference_cache_.find(voice_description);
    if (cached != reference_cache_.end()) {
        return cached->second;
    }

    LOG(INFO, "openmoss-server") << "Designing reference voice for: " << voice_description << std::endl;
    Subprocess designer = spawn(voicegen_path_);
    std::string sample;
    try {
        const std::string base = "http://127.0.0.1:" + std::to_string(designer.port);
        bool ready = false;
        for (int attempt = 0; attempt < 3000 && !ready; ++attempt) {
            try {
                auto health = utils::HttpClient::get(
                    base + "/health", {}, 2, utils::HttpSecurityPolicy::TrustedLoopback);
                ready = (health.status_code == 200);
            } catch (const std::exception&) {
                // Connection refused until the subprocess binds its port.
                ready = false;
            }
            if (!ready) std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        if (!ready) {
            throw std::runtime_error("voice-design backend failed to become ready");
        }
        json body;
        body["input"] = kVoiceDesignPhrase;
        body["voice"] = voice_description;
        body["response_format"] = "wav";
        auto response = utils::HttpClient::post(
            base + "/v1/audio/speech", body.dump(),
            {{"Content-Type", "application/json"}}, 600,
            utils::HttpSecurityPolicy::TrustedLoopback);
        if (response.status_code != 200 || response.body.empty()) {
            throw std::runtime_error("voice design failed: HTTP " + std::to_string(response.status_code));
        }
        sample = utils::JsonUtils::base64_encode(response.body);
    } catch (...) {
        utils::ProcessManager::stop_process(designer.handle);
        throw;
    }
    utils::ProcessManager::stop_process(designer.handle);
    LOG(INFO, "openmoss-server") << "Voice-design subprocess released" << std::endl;

    reference_cache_[voice_description] = sample;
    return sample;
}

json OpenMossServer::apply_voice_design(const json& request) {
    json forwarded = request;
    if (voicegen_path_.empty() || forwarded.contains("reference_wav_b64")) {
        return forwarded;
    }
    const std::string description = forwarded.value("voice", std::string());
    if (description.empty()) {
        return forwarded;
    }
    forwarded["reference_wav_b64"] = design_reference_sample(description);
    forwarded.erase("voice");
    return forwarded;
}

void OpenMossServer::audio_speech(const json& request, httplib::DataSink& sink) {
    json forwarded = apply_voice_design(request);

    // The server only derives a length bound for n_vq < 32; the delay family
    // reports 32 and gets none, so a one-line prompt can run to max_new_tokens
    // and emit minutes of audio. Bound it from the text unless the caller said
    // otherwise. ~12.5 frames/s at ~2.5 words/s is ~5 frames per word.
    if (!forwarded.contains("max_audio_frames") && !forwarded.contains("token_count")) {
        const std::string input = forwarded.value("input", std::string());
        if (!input.empty()) {
            int words = 1;
            for (char c : input) {
                if (c == ' ' || c == '\n' || c == '\t') ++words;
            }
            const int tokens = std::max(40, std::min(1000, words * 5));
            forwarded["max_audio_frames"] = std::max(48, tokens * 3 / 2);
        }
    }

    forward_streaming_request("/v1/audio/speech", forwarded.dump(), sink, /*sse=*/false, /*timeout_seconds=*/600);
}

void OpenMossServer::audio_generations(const json& request, httplib::DataSink& sink) {
    json body;
    body["prompt"] = request.value("prompt", std::string());
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
