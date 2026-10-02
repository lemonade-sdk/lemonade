/**
 * HRX reuses llama-server's protocol but supports only its local-GGUF chat
 * startup branch. HF loading, vision/mmproj, drafting, embeddings, reranking,
 * and generic GPU-runtime setup stay omitted because the qualified bundle owns
 * those choices. The test-visible launch builders remain local until both HRX
 * and llama.cpp can share a startup seam without either recipe's policy leaking
 * into the other.
 */

#include "lemon/backends/hrx/hrx_server.h"

#include "lemon/backend_manager.h"
#include "lemon/backends/backend_ops.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backends/hrx/hrx.h"
#include "lemon/model_manager.h"
#include "lemon/utils/custom_args.h"
#include "lemon/utils/process_manager.h"
#include "lemon/utils/recipe_arg_resolver.h"

#include <lemon/utils/aixlog.hpp>

#include <stdexcept>
#include <vector>

namespace lemon {
namespace backends {

namespace {

void validate_supported_build_host() {
#if !defined(__linux__) || (!defined(__x86_64__) && !defined(__amd64__))
    throw std::runtime_error(
        "The HRX backend is available on Linux x86-64 only");
#endif
}

}  // namespace

namespace hrx {

std::vector<std::string> build_server_argv(const std::string& gguf_path,
                                           int ctx_size,
                                           int port,
                                           const std::string& hrx_args) {
    std::vector<std::string> argv = {
        "-m", gguf_path,
        "--ctx-size", std::to_string(ctx_size),
        "--device", "HRX0",
        "--port", std::to_string(port),
        "--jinja",
        "--metrics",
    };

    if (!hrx_args.empty()) {
        const std::string validation_error =
            utils::validate_custom_args(hrx_args, reserved_custom_arg_flags());
        if (!validation_error.empty()) {
            throw std::invalid_argument(
                "Invalid custom HRX llama-server arguments:\n" +
                validation_error);
        }
    }

    // An auto slot count enables the unified KV buffer, which advertises the
    // full ctx_size to every slot instead of dividing it. Pinning the count
    // keeps ctx_size the context a request actually gets (llamacpp pins the
    // same default in resolve_llamacpp_runtime_args).
    const std::string resolved_args = utils::append_runtime_arg_defaults(
        hrx_args, {{"--parallel 1", "--parallel", {"-np"}}});

    const std::vector<std::string> custom_args =
        utils::parse_custom_args(resolved_args);
    argv.insert(argv.end(), custom_args.begin(), custom_args.end());
    return argv;
}

std::vector<std::pair<std::string, std::string>> build_server_environment() {
    return {{"GGML_DISABLE_VULKAN", "1"}};
}

}  // namespace hrx

InstallParams HrxServer::get_install_params(const std::string& backend,
                                            const std::string& version) {
    if (backend != "hrx") {
        throw std::invalid_argument(
            "Unsupported HRX backend '" + backend + "'; expected 'hrx'");
    }

    validate_supported_build_host();

    InstallParams params;
    params.repo = "ROCm/ggml-staging-automation";
    params.filename =
        "llama-" + version + "-bin-manylinux-hrx-x64.tar.gz";
    return params;
}

void HrxServer::load(const std::string& model_name,
                     const ModelInfo& model_info,
                     const RecipeOptions& options,
                     bool do_not_upgrade) {
    (void)do_not_upgrade;
    validate_supported_build_host();

    LOG(INFO, "HRX") << "Loading model: " << model_name << std::endl;
    LOG(DEBUG, "HRX") << "Per-model settings: "
                       << options.to_log_string() << std::endl;

    backend_manager_->install_backend(hrx::spec()->recipe, "hrx");

    const std::string gguf_path = model_info.resolved_path();
    if (gguf_path.empty()) {
        throw std::runtime_error(
            "GGUF file not found for checkpoint: " + model_info.checkpoint());
    }

    const int ctx_size = options.get_option("ctx_size");
    const std::string hrx_args = options.get_option("hrx_args");
    const int backend_port = choose_port();
    const std::string executable =
        BackendUtils::get_backend_binary_path(*hrx::spec(), "hrx");
    const std::vector<std::string> argv =
        hrx::build_server_argv(gguf_path, ctx_size, backend_port, hrx_args);
    const std::vector<std::pair<std::string, std::string>> environment =
        hrx::build_server_environment();

    const bool info_logging_enabled = log_level_ == "info";
    const bool inherit_output = info_logging_enabled || is_debug();
    set_process_handle(
        utils::ProcessManager::start_process(
            executable,
            argv,
            "",
            inherit_output,
            true,
            environment),
        executable,
        argv);

    if (!wait_for_ready("/health")) {
        const ProcessHandle handle = consume_process_handle_for_cleanup();
        if (has_process_handle(handle)) {
            utils::ProcessManager::stop_process(handle);
        }
        throw std::runtime_error("HRX llama-server failed to start");
    }

    LOG(DEBUG, "HRX") << "Model loaded on port "
                       << get_backend_port() << std::endl;
}

}  // namespace backends
}  // namespace lemon

namespace lemon {
namespace backends {

namespace {

class HrxOps : public BackendOps {
public:
    void populate_metadata(ModelInfo& info,
                           const BackendOpsContext& ctx) const override {
        llamacpp::ops()->populate_metadata(info, ctx);
    }

    std::string resolve_checkpoint_path(
        const ModelInfo& info,
        const CheckpointResolveContext& ctx) const override {
        return llamacpp::ops()->resolve_checkpoint_path(info, ctx);
    }

    std::string find_imported_checkpoint(
        const std::string& import_dir) const override {
        return llamacpp::ops()->find_imported_checkpoint(import_dir);
    }

    std::string validate_registration_checkpoint(
        const std::string& checkpoint) const override {
        return llamacpp::ops()->validate_registration_checkpoint(checkpoint);
    }

    std::string validate_checkpoint_file(
        const std::string& resolved_path) const override {
        return llamacpp::ops()->validate_checkpoint_file(resolved_path);
    }
};

}  // namespace

namespace hrx {

std::unique_ptr<WrappedServer> create(const BackendContext& ctx) {
    return make_server<HrxServer>(ctx);
}

const BackendSpec* spec() {
    return make_spec<HrxServer>(descriptor);
}

const BackendOps* ops() {
    return single_ops<HrxOps>();
}

}  // namespace hrx
}  // namespace backends
}  // namespace lemon
