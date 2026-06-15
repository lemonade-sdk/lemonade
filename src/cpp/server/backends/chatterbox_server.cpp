#include "lemon/backends/chatterbox_server.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backend_manager.h"
#include "lemon/error_types.h"
#include "lemon/runtime_config.h"
#include "lemon/system_info.h"
#include "lemon/utils/custom_args.h"
#include "lemon/utils/process_manager.h"
#include <httplib.h>
#include <filesystem>
#include <iostream>
#include <set>
#include <vector>
#include <lemon/utils/aixlog.hpp>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace lemon::utils;

namespace lemon {
namespace backends {

namespace {
// Map the resolved backend (cuda/rocm/metal/cpu) to the torch device string the
// chatterbox-server wrapper expects. PyTorch's ROCm build exposes AMD GPUs
// through the CUDA API, so "rocm" also maps to "cuda".
std::string backend_to_device(const std::string& backend) {
    if (backend == "metal") {
        return "mps";
    }
    if (backend == "cpu") {
        return "cpu";
    }
    return "cuda";  // cuda (NVIDIA) and rocm (AMD via HIP)
}
}  // namespace

InstallParams ChatterboxServer::get_install_params(const std::string& backend, const std::string& version) {
    InstallParams params;
    params.repo = "lemonade-sdk/chatterbox-rocm";

    // Self-contained PyInstaller bundles built by the lemonade-sdk/chatterbox-rocm
    // distribution repo (tracks chatterbox-tts PyPI releases; tag scheme
    // chatterbox<version>) — no system Python needed. One PyTorch wheel covers
    // all GPU architectures, so assets vary only by OS and device variant
    // (cuda/rocm/metal/cpu), not by sm_/gfx arch.
    //
    // Device availability per platform:
    //   windows x64  : cuda, cpu
    //   linux   x64  : cuda, rocm, cpu
    //   macos   arm64: metal, cpu
#ifdef _WIN32
    params.filename = "chatterbox-server-" + version + "-windows-x64-" + backend + ".zip";
#elif defined(__APPLE__)
    params.filename = "chatterbox-server-" + version + "-macos-arm64-" + backend + ".tar.gz";
#else
    params.filename = "chatterbox-server-" + version + "-linux-x64-" + backend + ".tar.gz";
#endif

    return params;
}

ChatterboxServer::ChatterboxServer(const std::string& log_level, ModelManager* model_manager,
                                   BackendManager* backend_manager)
    : WrappedServer("chatterbox-server", log_level, model_manager, backend_manager) {
}

ChatterboxServer::~ChatterboxServer() {
    unload();
}

void ChatterboxServer::load(const std::string& model_name, const ModelInfo& model_info,
                            const RecipeOptions& options, bool do_not_upgrade) {
    (void)do_not_upgrade;
    LOG(INFO, "ChatterboxServer") << "Loading model: " << model_name << std::endl;
    LOG(DEBUG, "ChatterboxServer") << "Per-model settings: " << options.to_log_string() << std::endl;

    // Resolve the device backend. An empty/"auto" option resolves to the first
    // supported backend in RECIPE_DEFS preference order (GPU when present, else
    // CPU). A user/config override is validated against the supported set.
    std::string backend_option = options.get_option("chatterbox_backend");
    RuntimeConfig::validate_backend_choice("chatterbox", backend_option);
    std::string backend = backend_option;
    if (backend.empty() || backend == "auto") {
        auto supported = SystemInfo::get_supported_backends("chatterbox");
        if (supported.backends.empty()) {
            throw std::runtime_error(
                supported.not_supported_error.empty()
                    ? SystemInfo::get_unsupported_backend_error("chatterbox", "auto")
                    : supported.not_supported_error);
        }
        backend = supported.backends[0];
    }
    LOG(INFO, "ChatterboxServer") << "Using backend: " << backend << std::endl;

    // get_device_type_from_recipe() defaults chatterbox to GPU; the cpu backend runs on CPU.
    device_type_ = (backend == "cpu") ? DEVICE_CPU : DEVICE_GPU;

    // Install chatterbox-server (device-specific bundle) if needed.
    backend_manager_->install_backend(SPEC.recipe, backend);

    // Resolve the checkpoint directory (HF snapshot) downloaded by Lemonade.
    std::string ckpt_dir = model_info.resolved_path();
    if (ckpt_dir.empty() || !fs::exists(ckpt_dir)) {
        throw std::runtime_error("Model directory not found for checkpoint: " + model_info.checkpoint());
    }

    std::string variant = model_info.chatterbox_variant.empty() ? "english" : model_info.chatterbox_variant;
    std::string device = backend_to_device(backend);

    std::string exe_path = BackendUtils::get_backend_binary_path(SPEC, backend);
    LOG(INFO, "ChatterboxServer") << "Using executable: " << exe_path << std::endl;

    port_ = choose_port();
    if (port_ == 0) {
        throw std::runtime_error("Failed to find an available port");
    }
    LOG(INFO, "ChatterboxServer") << "Starting server on port " << port_
                                  << " (variant=" << variant << ", device=" << device << ")" << std::endl;

    // Lemonade manages the model path, variant, device, and port; optional
    // chatterbox-server flags come from chatterbox_args.
    std::vector<std::string> args = {
        "--ckpt-dir", ckpt_dir,
        "--variant", variant,
        "--device", device,
        "--host", "127.0.0.1",
        "--port", std::to_string(port_)
    };

    std::string chatterbox_args = options.get_option("chatterbox_args");
    if (!chatterbox_args.empty()) {
        std::set<std::string> reserved_flags = {
            "--ckpt-dir", "--variant", "--device", "--host", "--port"
        };
        std::string validation_error = validate_custom_args(chatterbox_args, reserved_flags);
        if (!validation_error.empty()) {
            throw std::invalid_argument(
                "Invalid custom chatterbox-server arguments:\n" + validation_error);
        }
        std::vector<std::string> custom_args_vec = parse_custom_args(chatterbox_args);
        args.insert(args.end(), custom_args_vec.begin(), custom_args_vec.end());
    }

    // Prevent system/user Python packages from leaking into the bundled environment.
    std::vector<std::pair<std::string, std::string>> env_vars;
    env_vars.push_back({"PYTHONNOUSERSITE", "1"});

    ProcessHandle started_handle = utils::ProcessManager::start_process(
        exe_path,
        args,
        "",          // working_dir
        is_debug(),  // inherit_output
        false,
        env_vars
    );
    set_process_handle(started_handle);

    if (!has_process_handle(started_handle)) {
        throw std::runtime_error("Failed to start chatterbox-server process");
    }
    LOG(INFO, "ChatterboxServer") << "Process started with PID: " << started_handle.pid << std::endl;

    // Model load can be slow (downloads/weights init); wait_for_ready polls
    // /health, which returns "starting" until the model is loaded.
    if (!wait_for_ready("/health")) {
        unload();
        throw std::runtime_error("chatterbox-server failed to start or become ready");
    }
    LOG(INFO, "ChatterboxServer") << "Server is ready!" << std::endl;
}

void ChatterboxServer::unload() {
    stop_backend_watchdog();
    const ProcessHandle handle = consume_process_handle_for_cleanup();
    if (has_process_handle(handle)) {
        LOG(INFO, "ChatterboxServer") << "Stopping server (PID: " << handle.pid << ")" << std::endl;
        utils::ProcessManager::stop_process(handle);
    }
}

// ICompletionServer implementation (not supported - return errors)
json ChatterboxServer::chat_completion(const json& request) {
    (void)request;
    return json{
        {"error", {
            {"message", "Chatterbox does not support text completion. Use audio speech endpoints instead."},
            {"type", "unsupported_operation"},
            {"code", "model_not_applicable"}
        }}
    };
}

json ChatterboxServer::completion(const json& request) {
    (void)request;
    return json{
        {"error", {
            {"message", "Chatterbox does not support text completion. Use audio speech endpoints instead."},
            {"type", "unsupported_operation"},
            {"code", "model_not_applicable"}
        }}
    };
}

json ChatterboxServer::responses(const json& request) {
    (void)request;
    return json{
        {"error", {
            {"message", "Chatterbox does not support text completion. Use audio speech endpoints instead."},
            {"type", "unsupported_operation"},
            {"code", "model_not_applicable"}
        }}
    };
}

void ChatterboxServer::audio_speech(const json& request, httplib::DataSink& sink) {
    json tts_request = request;
    tts_request["model"] = "chatterbox";

    // OpenAI does not define "stream" for the speech endpoint, relying solely on
    // stream_format. The wrapper honors stream_format but we also set the
    // boolean for parity with the Kokoro contract.
    if (request.contains("stream_format")) {
        tts_request["stream"] = true;
    }

    forward_streaming_request("/v1/audio/speech", tts_request.dump(), sink, false);
}

} // namespace backends
} // namespace lemon
