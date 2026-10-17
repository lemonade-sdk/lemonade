#include "lemon/backends/rocmfpx/rocmfpx_server.h"

#include "lemon/backend_manager.h"
#include "lemon/backends/backend_ops.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backends/llamacpp/llamacpp_server.h"
#include "lemon/backends/rocmfpx/rocmfpx.h"
#include "lemon/gguf_shard_utils.h"
#include "lemon/model_manager.h"
#include "lemon/utils/custom_args.h"
#include "lemon/utils/recipe_arg_resolver.h"

#include <lemon/utils/aixlog.hpp>

#include <stdexcept>
#include <vector>

namespace lemon {
namespace backends {

namespace {

constexpr const char* kBackend = "rocm";

class RocmFpxOps : public BackendOps {
public:
    void populate_metadata(ModelInfo& info, const BackendOpsContext& ctx) const override {
        llamacpp::ops()->populate_metadata(info, ctx);
    }

    std::string resolve_checkpoint_path(const ModelInfo& info,
                                        const CheckpointResolveContext& ctx) const override {
        return llamacpp::ops()->resolve_checkpoint_path(info, ctx);
    }

    std::string find_imported_checkpoint(const std::string& import_dir) const override {
        return llamacpp::ops()->find_imported_checkpoint(import_dir);
    }

    std::string validate_registration_checkpoint(const std::string& checkpoint) const override {
        return llamacpp::ops()->validate_registration_checkpoint(checkpoint);
    }

    std::string validate_checkpoint_file(const std::string& resolved_path) const override {
        return llamacpp::ops()->validate_checkpoint_file(resolved_path);
    }
};

}  // namespace

namespace rocmfpx {

std::vector<std::string> build_server_argv(const std::string& gguf_path,
                                           const std::string& mmproj_path,
                                           const std::string& draft_path,
                                           bool mtp,
                                           int ctx_size,
                                           int port,
                                           const std::string& rocmfpx_args) {
    std::vector<std::string> argv = {
        "-m", gguf_path,
        "--ctx-size", std::to_string(ctx_size),
        "--port", std::to_string(port),
        "--host", "0.0.0.0",
        "--jinja",
        "--metrics",
    };
    if (!mmproj_path.empty()) {
        argv.insert(argv.end(), {"--mmproj", mmproj_path});
    }
    if (!draft_path.empty()) {
        argv.insert(argv.end(), {"--model-draft", draft_path});
    }

    if (!rocmfpx_args.empty()) {
        const std::string validation_error =
            utils::validate_custom_args(rocmfpx_args, reserved_custom_arg_flags());
        if (!validation_error.empty()) {
            throw std::invalid_argument("Invalid custom ROCm FPX llama-server arguments:\n" +
                                        validation_error);
        }
    }

    // The same defaults llamacpp gives its models, each yielding to a user's
    // own copy (see resolve_llamacpp_runtime_args).
    std::vector<utils::RuntimeArgDefault> defaults;
    if (mtp) {
        defaults.push_back({"--spec-type draft-mtp", "--spec-type"});
    }
    defaults.push_back({"--parallel 1", "--parallel", {"-np"}});
    const std::vector<std::string> custom_args =
        utils::parse_custom_args(utils::append_runtime_arg_defaults(rocmfpx_args, defaults));
    argv.insert(argv.end(), custom_args.begin(), custom_args.end());
    return argv;
}

}  // namespace rocmfpx

void RocmFpxServer::load(const std::string& model_name,
                         const ModelInfo& model_info,
                         const RecipeOptions& options,
                         bool do_not_upgrade) {
    (void)do_not_upgrade;  // install_backend() is a no-op once the pinned image is present
    LOG(INFO, "RocmFpx") << "Loading model: " << model_name << std::endl;

    backend_manager_->install_backend(rocmfpx::descriptor.recipe, kBackend);

    const std::string gguf_path = model_info.resolved_path();
    if (gguf_path.empty()) {
        throw std::runtime_error("GGUF file not found for checkpoint: " + model_info.checkpoint());
    }
    const std::string mmproj_path = model_info.resolved_path("mmproj");
    const std::string draft_path = model_info.resolved_path("draft");

    const int ctx_size = options.get_option("ctx_size");
    const std::string rocmfpx_args = options.get_option("rocmfpx_args");
    device_type_ = DEVICE_GPU;
    port_ = choose_port();

    ServerCommand command;
    command.program = rocmfpx::descriptor.binary;
    command.args = rocmfpx::build_server_argv(gguf_path, mmproj_path, draft_path,
                                              has_label(model_info.labels, "mtp"), ctx_size, port_,
                                              rocmfpx_args);
    for (const std::string& path : {gguf_path, mmproj_path, draft_path}) {
        if (!path.empty()) {
            const auto files = gguf_files(path);
            command.model_files.insert(command.model_files.end(), files.begin(), files.end());
        }
    }
    command.port = port_;

    const bool inherit_output = (log_level_ == "info") || is_debug();
    start_server(std::make_unique<ContainerProcess>(
                     ProcessOutput{inherit_output, true}, rocmfpx::descriptor.recipe, kBackend,
                     model_name, *rocmfpx::descriptor.container_for(kBackend),
                     BackendUtils::get_backend_image(rocmfpx::descriptor.recipe, kBackend)),
                 command);
    LOG(DEBUG, "RocmFpx") << "Model loaded on port " << get_backend_port() << std::endl;
}

namespace rocmfpx {

std::unique_ptr<WrappedServer> create(const BackendContext& ctx) {
    return make_server<RocmFpxServer>(ctx);
}

const BackendSpec* spec() {
    static const BackendSpec kSpec(descriptor.recipe, descriptor.binary);
    return &kSpec;
}

const BackendOps* ops() {
    return single_ops<RocmFpxOps>();
}

}  // namespace rocmfpx
}  // namespace backends
}  // namespace lemon
