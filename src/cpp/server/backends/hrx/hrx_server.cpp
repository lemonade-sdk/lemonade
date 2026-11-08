/**
 * HRX reuses llama-server's protocol but supports only its local-GGUF chat
 * startup branch, including embedded MTP heads. HF loading, vision/mmproj,
 * separate draft checkpoints, embeddings, reranking,
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
#include "lemon/utils/recipe_arg_resolver.h"

#include <lemon/utils/aixlog.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
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

namespace {

constexpr int64_t kQwen38HrxContextLimit = 32768;

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool is_qwen38_27b(const ModelInfo& model_info) {
    return lower_ascii(model_info.model_name).find("qwen3.8-27b") != std::string::npos ||
           lower_ascii(model_info.checkpoint()).find("qwen3.8-27b") != std::string::npos;
}

int64_t effective_context_window(const ModelInfo& model_info,
                                 int64_t detected_context_window) {
    if (!is_qwen38_27b(model_info)) {
        return detected_context_window;
    }
    if (detected_context_window <= 0 ||
        detected_context_window > kQwen38HrxContextLimit) {
        return kQwen38HrxContextLimit;
    }
    return detected_context_window;
}

void clamp_context_size(const ModelInfo& model_info, RecipeOptions& options) {
    if (!is_qwen38_27b(model_info)) {
        return;
    }

    const int64_t context_limit =
        effective_context_window(model_info, model_info.max_context_window);
    const json ctx_size = options.get_option("ctx_size");
    bool exceeds_context_limit = false;
    if (ctx_size.is_number()) {
        const double requested_ctx_size = ctx_size.get<double>();
        exceeds_context_limit =
            requested_ctx_size == 0 ||
            requested_ctx_size > static_cast<double>(context_limit);
    }
    if (exceeds_context_limit) {
        options.set_option("ctx_size", context_limit);
    }
}

}  // namespace

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

    ServerCommand command;
    command.program = executable;
    command.args = argv;
    command.env = environment;
    command.port = backend_port;

    const bool info_logging_enabled = log_level_ == "info";
    const bool inherit_output = info_logging_enabled || is_debug();
    start_server(std::make_unique<NativeProcess>(ProcessOutput{inherit_output, true}), command);
}

}  // namespace backends
}  // namespace lemon

namespace lemon {
namespace backends {

namespace {

class HrxOps : public BackendOps {
public:
    void resolve_runtime_options(const ModelInfo& info, RecipeOptions& options) const override {
        const json merge_args_value = options.get_option("merge_args");
        const bool merge_args =
            merge_args_value.is_boolean() ? merge_args_value.get<bool>() : true;
        if (!merge_args) return;

        const bool uses_mtp =
            std::find(info.labels.begin(), info.labels.end(), "mtp") != info.labels.end();
        if (!uses_mtp) return;

        const json custom_args_value = options.get_option("hrx_args");
        const std::string custom_args =
            custom_args_value.is_string() ? custom_args_value.get<std::string>() : "";
        options.set_option("hrx_args", utils::append_runtime_arg_defaults(
            custom_args, {{"--spec-type draft-mtp", "--spec-type"}}));
    }

    void populate_metadata(ModelInfo& info,
                           const BackendOpsContext& ctx) const override {
        llamacpp::ops()->populate_metadata(info, ctx);
        info.max_context_window =
            hrx::effective_context_window(info, info.max_context_window);
    }

    void resolve_runtime_options(const ModelInfo& info,
                                 RecipeOptions& options) const override {
        llamacpp::ops()->resolve_runtime_options(info, options);
        hrx::clamp_context_size(info, options);
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
