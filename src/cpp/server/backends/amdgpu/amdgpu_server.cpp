#include "lemon/backends/amdgpu/amdgpu_server.h"

#include "lemon/backend_manager.h"
#include "lemon/backends/amdgpu/amdgpu.h"
#include "lemon/backends/backend_ops.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backends/ryzenai/ryzenai_server.h"
#include "lemon/model_manager.h"
#include "lemon/utils/path_utils.h"

#include <lemon/utils/aixlog.hpp>

#include <filesystem>
#include <stdexcept>

namespace lemon {
namespace backends {

namespace {

constexpr const char* kBackend = "gpu";

// A model is an ONNX Runtime GenAI directory, the layout ryzenai-llm resolves.
class AmdGpuOps : public BackendOps {
public:
    std::string resolve_checkpoint_path(const ModelInfo& info,
                                        const CheckpointResolveContext& ctx) const override {
        return ryzenai::ops()->resolve_checkpoint_path(info, ctx);
    }

    std::string find_imported_checkpoint(const std::string& import_dir) const override {
        return ryzenai::ops()->find_imported_checkpoint(import_dir);
    }
};

}  // namespace

InstallParams AmdGpuServer::get_install_params(const std::string& backend,
                                               const std::string& /*version*/) {
#ifdef _WIN32
    if (backend == kBackend) {
        return {"lemonade-sdk/amdgpu-server", "amdgpu-server-windows-x64.zip"};
    }
#endif
    throw std::runtime_error("amdgpu-llm:" + backend + " has no release download for this platform");
}

AmdGpuServer::AmdGpuServer(const std::string& log_level, ModelManager* model_manager,
                           BackendManager* backend_manager)
    : WrappedServer("amdgpu-server", log_level, model_manager, backend_manager) {}

void AmdGpuServer::load(const std::string& model_name, const ModelInfo& model_info,
                        const RecipeOptions& options, bool /*do_not_upgrade*/) {
    LOG(INFO, "AMDGPU") << "Loading model: " << model_name << std::endl;

    backend_manager_->install_backend(amdgpu::descriptor.recipe, kBackend);

    const std::string model_path = model_info.resolved_path();
    if (model_path.empty() || !std::filesystem::exists(utils::path_from_utf8(model_path))) {
        throw std::runtime_error("Model path does not exist: " + model_path);
    }

    const int ctx_size = options.get_option("ctx_size");
    device_type_ = DEVICE_GPU;
    port_ = choose_port();

    ServerCommand command;
    command.program = BackendUtils::get_backend_binary_path(*amdgpu::spec(), kBackend);
    command.args = {
        "-m", model_path,
        "--port", std::to_string(port_),
        "--ctx-size", std::to_string(ctx_size),
    };
    if (is_debug()) {
        command.args.push_back("--verbose");
    }
    command.port = port_;

    // amdgpu-server prints the start of every prompt even without --verbose,
    // so its output only reaches the log at debug level.
    start_server(std::make_unique<NativeProcess>(ProcessOutput{is_debug(), true}), command);
    LOG(DEBUG, "AMDGPU") << "Model loaded on port " << get_backend_port() << std::endl;
}

json AmdGpuServer::chat_completion(const json& request) {
    return forward_request("/v1/chat/completions", request);
}

json AmdGpuServer::completion(const json& request) {
    return forward_request("/v1/completions", request);
}

json AmdGpuServer::responses(const json& request) {
    return forward_request("/v1/responses", request);
}

namespace amdgpu {

std::unique_ptr<WrappedServer> create(const BackendContext& ctx) {
    return make_server<AmdGpuServer>(ctx);
}

const BackendSpec* spec() {
    return make_spec<AmdGpuServer>(descriptor);
}

const BackendOps* ops() {
    return single_ops<AmdGpuOps>();
}

}  // namespace amdgpu
}  // namespace backends
}  // namespace lemon
