#include "lemon/backends/voxcpm2/voxcpm2_server.h"
#include "lemon/backends/voxcpm2/voxcpm2.h"
#include "lemon/backends/backend_registry.h"
#include "lemon/backends/backend_ops.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backend_manager.h"
#include "lemon/error_types.h"
#include "lemon/model_manager.h"
#include "lemon/runtime_config.h"
#include "lemon/system_info.h"
#include "lemon/utils/process_manager.h"
#include <lemon/utils/aixlog.hpp>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace lemon {
namespace backends {

InstallParams VoxCPM2Server::get_install_params(const std::string& backend, const std::string& version) {
    (void)version;
    InstallParams params;
    // Upstream llama.cpp-omni publishes only the Comni desktop installers, so the
    // per-variant llama-tts-server archives are built and released from a fork.
    params.repo = "ZMXJJ/llama.cpp-omni";
#ifdef _WIN32
    params.filename = "llama-tts-server-" + backend + "-windows-x64.zip";
#elif defined(__APPLE__)
    params.filename = "llama-tts-server-" + backend + "-macos-arm64.tar.gz";
#else
    params.filename = "llama-tts-server-" + backend + "-linux-x64.tar.gz";
#endif
    return params;
}

VoxCPM2Server::VoxCPM2Server(const std::string& log_level,
                             ModelManager* model_manager,
                             BackendManager* backend_manager)
    : WrappedServer("voxcpm2-server", log_level, model_manager, backend_manager) {}

VoxCPM2Server::~VoxCPM2Server() {
    unload();
}

std::string VoxCPM2Server::resolve_binary_path(const std::string& backend) {
    const BackendSpec* spec = voxcpm2::spec();
    std::string external = BackendUtils::find_external_backend_binary(spec->recipe, backend);
    if (!external.empty() && std::filesystem::exists(external)) {
        return external;
    }
    backend_manager_->install_backend(spec->recipe, backend);
    return BackendUtils::get_backend_binary_path(*spec, backend);
}

std::string VoxCPM2Server::resolve_backend(const RecipeOptions& options) const {
    std::string backend = options.get_option("voxcpm2_backend");
    if (!backend.empty()) {
        return backend;
    }
    auto supported = SystemInfo::get_supported_backends("voxcpm2");
    return supported.backends.empty() ? std::string() : supported.backends[0];
}

DeviceType VoxCPM2Server::effective_device(const RecipeOptions& options) const {
    const std::string backend = resolve_backend(options);
    if (backend.empty()) {
        return WrappedServer::effective_device(options);
    }
    return backend == "cpu" ? DEVICE_CPU : DEVICE_GPU;
}

void VoxCPM2Server::load(const std::string& model_name,
                         const ModelInfo& model_info,
                         const RecipeOptions& options,
                         bool /*do_not_upgrade*/) {
    LOG(INFO, "voxcpm2-server") << "Loading model: " << model_name << std::endl;

    const std::string base_lm_path = model_info.resolved_path("main");
    if (base_lm_path.empty() || !std::filesystem::exists(base_lm_path)) {
        throw std::runtime_error("BaseLM path not found for checkpoint: " + model_info.checkpoint("main"));
    }

    const std::string acoustic_path = model_info.resolved_path("acoustic");
    if (acoustic_path.empty() || !std::filesystem::exists(acoustic_path)) {
        throw std::runtime_error("Acoustic path not found for checkpoint: " + model_info.checkpoint("acoustic"));
    }

    const std::string backend = resolve_backend(options);
    if (backend.empty()) {
        throw UnsupportedOperationException(
            "VoxCPM2", "this system: no supported backend (Metal, Vulkan, CUDA, or CPU) detected");
    }
    RuntimeConfig::validate_backend_choice("voxcpm2", backend);
    const std::string exe_path = resolve_binary_path(backend);

    port_ = choose_port();
    if (port_ == 0) {
        throw std::runtime_error("Failed to find an available port");
    }

    std::vector<std::string> args = {
        "--voxcpm2-base-lm", base_lm_path,
        "--voxcpm2-acoustic", acoustic_path,
        "--host", "127.0.0.1",
        "--port", std::to_string(port_),
    };

    std::vector<std::pair<std::string, std::string>> env_vars;
    if (backend == "cuda") {
        const std::string exe_dir = std::filesystem::path(exe_path).parent_path().string();
#ifdef _WIN32
        std::string path = exe_dir;
        if (const char* p = std::getenv("PATH")) path += std::string(";") + p;
        env_vars.push_back({"PATH", path});
#else
        std::string ld = exe_dir;
        if (const char* p = std::getenv("LD_LIBRARY_PATH")) ld += std::string(":") + p;
        env_vars.push_back({"LD_LIBRARY_PATH", ld});
#endif
        BackendUtils::apply_cuda_env_vars(env_vars, "voxcpm2-server");
    }

    LOG(INFO, "voxcpm2-server") << "Starting " << exe_path << " on port " << port_ << std::endl;
    ProcessHandle started_handle = utils::ProcessManager::start_process(
        exe_path, args, "", is_debug(), false, env_vars);
    set_process_handle(started_handle);
    if (!has_process_handle(started_handle)) {
        throw std::runtime_error("Failed to start llama-tts-server process");
    }
    LOG(INFO, "voxcpm2-server") << "Process started with PID: " << started_handle.pid << std::endl;

    if (!wait_for_ready("/health")) {
        unload();
        throw std::runtime_error("llama-tts-server failed to start or become ready");
    }
}

void VoxCPM2Server::unload() {
    stop_backend_watchdog();
    const ProcessHandle handle = consume_process_handle_for_cleanup();
    if (has_process_handle(handle)) {
        LOG(INFO, "voxcpm2-server") << "Stopping server (PID: " << handle.pid << ")" << std::endl;
        utils::ProcessManager::stop_process(handle);
    }
}

void VoxCPM2Server::audio_speech(const json& request, httplib::DataSink& sink) {
    json forwarded = request;
    // llama-tts-server validates "model" against its own engine name and rejects
    // anything else, so the catalog name Lemonade routed on cannot be passed through.
    forwarded["model"] = "voxcpm2";

    // Streaming requests deliberately go to the buffered endpoint.
    // /v1/audio/speech/stream re-decodes a context window of latents on every
    // step, which costs more than one buffered decode of the whole utterance,
    // so the extra wall-clock outweighs starting playback earlier. Switch once
    // that gap closes.
    forward_streaming_request("/v1/audio/speech", forwarded.dump(), sink, /*sse=*/false, /*timeout_seconds=*/600);
}

}  // namespace backends

namespace backends {
namespace voxcpm2 {

std::unique_ptr<WrappedServer> create(const BackendContext& ctx) {
    return make_server<VoxCPM2Server>(ctx);
}

const BackendSpec* spec() { return make_spec<VoxCPM2Server>(descriptor); }
const BackendOps* ops() { return default_backend_ops(); }

}  // namespace voxcpm2
}  // namespace backends
}  // namespace lemon
