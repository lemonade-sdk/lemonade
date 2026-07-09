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
#include "lemon/utils/process_manager.h"
#include <lemon/utils/aixlog.hpp>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace lemon {
namespace backends {

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

    port_ = choose_port();
    if (port_ == 0) {
        throw std::runtime_error("Failed to find an available port");
    }

    std::vector<std::string> args = {
        "--model", model_path,
        "--host", "127.0.0.1",
        "--port", std::to_string(port_),
        "--no-webui",
    };

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

    LOG(INFO, "openmoss-server") << "Starting " << exe_path << " on port " << port_ << std::endl;
    ProcessHandle started_handle = utils::ProcessManager::start_process(
        exe_path, args, "", is_debug(), false, env_vars);
    set_process_handle(started_handle);
    if (!has_process_handle(started_handle)) {
        throw std::runtime_error("Failed to start openmoss-server process");
    }
    LOG(INFO, "openmoss-server") << "Process started with PID: " << started_handle.pid << std::endl;

    if (!wait_for_ready("/health")) {
        unload();
        throw std::runtime_error("openmoss-server failed to start or become ready");
    }
}

void OpenMossServer::unload() {
    stop_backend_watchdog();
    const ProcessHandle handle = consume_process_handle_for_cleanup();
    if (has_process_handle(handle)) {
        LOG(INFO, "openmoss-server") << "Stopping server (PID: " << handle.pid << ")" << std::endl;
        utils::ProcessManager::stop_process(handle);
    }
}

void OpenMossServer::audio_speech(const json& request, httplib::DataSink& sink) {
    forward_streaming_request("/v1/audio/speech", request.dump(), sink, /*sse=*/false, /*timeout_seconds=*/600);
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
