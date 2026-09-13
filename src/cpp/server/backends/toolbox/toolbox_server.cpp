#include "lemon/backends/toolbox/toolbox_server.h"

#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include <lemon/utils/aixlog.hpp>
#include "lemon/backends/container_backend.h"
#include "lemon/backends/toolbox/toolbox.h"
#include "lemon/model_manager.h"
#include "lemon/runtime_config.h"
#include "lemon/system_info.h"
#include "lemon/utils/custom_args.h"
#include "lemon/utils/process_manager.h"

namespace fs = std::filesystem;
using namespace lemon::utils;

namespace lemon {
namespace backends {

namespace {

// The variant to run: the user's choice, else the most-preferred supported one
// (descriptor support rows are ordered by preference).
std::string resolve_variant(const RecipeOptions& options) {
    std::string variant = options.get_option("llamacpp-toolbox_backend");
    if (!variant.empty() && variant != "auto") {
        return variant;
    }
    auto supported = SystemInfo::get_supported_backends(toolbox::descriptor.recipe);
    return supported.backends.empty() ? "rocmfpx" : supported.backends[0];
}

}  // namespace

LlamaCppToolboxServer::LlamaCppToolboxServer(const std::string& log_level,
                                             ModelManager* model_manager,
                                             BackendManager* backend_manager)
    : LlamaCppServer(log_level, model_manager, backend_manager) {
    server_name_ = "llamacpp-toolbox";
}

LlamaCppToolboxServer::~LlamaCppToolboxServer() {
    unload();
}

void LlamaCppToolboxServer::load(const std::string& model_name, const ModelInfo& model_info,
                                 const RecipeOptions& options, bool do_not_upgrade) {
    (void)do_not_upgrade;  // install_backend() is a no-op once the pinned digest is present

    variant_ = resolve_variant(options);
    RuntimeConfig::validate_backend_choice(toolbox::descriptor.effective_config_section(),
                                           options.get_option("llamacpp-toolbox_backend"));

    LOG(INFO, "Toolbox") << "Loading " << model_name << " on toolbox variant " << variant_
                         << std::endl;

    int ctx_size = options.get_option("ctx_size");
    const std::string custom_args = options.get_option("toolbox_args");

    const std::string gguf_path = model_info.resolved_path();
    if (gguf_path.empty() || !fs::exists(gguf_path)) {
        throw std::runtime_error("llamacpp-toolbox: GGUF file not found for checkpoint: " +
                                 model_info.checkpoint());
    }
    const std::string mmproj_path = model_info.resolved_path("mmproj");

    device_type_ = DEVICE_GPU;
    backend_manager_->install_backend(toolbox::descriptor.recipe, variant_);

    port_ = choose_port();

    // A container left by a previous run (a killed lemond) would make `run`
    // fail on the name, so clear it before claiming the name again.
    clear_stale_container(toolbox::descriptor.recipe, variant_);

    ContainerLaunchRequest request;
    request.recipe = toolbox::descriptor.recipe;
    request.variant = variant_;
    request.profile_id = toolbox::profile_for(variant_);
    ContainerLaunchPlan plan = plan_container_launch(request, port_);

    const bool supports_embeddings = (model_info.type == ModelType::EMBEDDING);
    const bool supports_reranking = (model_info.type == ModelType::RERANKING);
    if (supports_embeddings && ctx_size < 8192) {
        ctx_size = 8192;
    }

    const toolbox::VariantDefaults tuned = toolbox::defaults_for(variant_);

    std::vector<std::string> command;
    command.push_back("llama-server");
    command.push_back("-m");
    command.push_back(plan.container_path(gguf_path));
    command.push_back("--host");
    command.push_back("0.0.0.0");  // reachable from the published loopback port only
    command.push_back("--port");
    command.push_back(std::to_string(port_));
    command.push_back("--ctx-size");
    command.push_back(std::to_string(ctx_size));
    command.push_back("--jinja");
    command.push_back("--metrics");
    command.push_back("--batch-size");
    command.push_back(std::to_string(tuned.batch_size));
    command.push_back("--ubatch-size");
    command.push_back(std::to_string(tuned.ubatch_size));
    if (tuned.flash_attention) {
        command.push_back("-fa");
        command.push_back("1");
    }
    if (tuned.no_mmap) {
        command.push_back("--no-mmap");
    }
    if (!mmproj_path.empty()) {
        command.push_back("--mmproj");
        command.push_back(plan.container_path(mmproj_path));
    }
    if (supports_embeddings) {
        command.push_back("--embeddings");
    }
    if (supports_reranking) {
        command.push_back("--reranking");
    }

    if (!custom_args.empty()) {
        const std::string validation_error =
            validate_custom_args(custom_args, toolbox::reserved_custom_arg_flags());
        if (!validation_error.empty()) {
            throw std::invalid_argument("Invalid custom toolbox arguments:\n" + validation_error);
        }
        const std::vector<std::string> parsed = parse_custom_args(custom_args);
        command.insert(command.end(), parsed.begin(), parsed.end());
    }

    plan.set_command(std::move(command));
    const std::vector<std::string> engine_args = plan.engine_args();

    LOG(INFO, "Toolbox") << "Starting " << plan.image().tagged_ref() << " as "
                         << plan.container_name() << " on port " << port_ << std::endl;

    const bool inherit_output = (log_level_ == "info") || is_debug();
    set_process_handle(ProcessManager::start_process(plan.engine_executable(), engine_args, "",
                                                     inherit_output, true, {}),
                       plan.engine_executable(), engine_args);

    if (!wait_for_ready("/health")) {
        unload();
        throw std::runtime_error("Toolbox llama-server failed to start");
    }

    LOG(DEBUG, "Toolbox") << "Model loaded on port " << get_backend_port() << std::endl;
}

void LlamaCppToolboxServer::unload() {
    stop_backend_watchdog();

    if (!variant_.empty()) {
        stop_container_for(toolbox::descriptor.recipe, variant_);
    }

    const ProcessHandle handle = consume_process_handle_for_cleanup();
    if (has_process_handle(handle)) {
        LOG(INFO, "Toolbox") << "Stopping toolbox container" << std::endl;
        ProcessManager::stop_process(handle);
    }
}

namespace toolbox {

std::unique_ptr<WrappedServer> create(const BackendContext& ctx) {
    return make_server<LlamaCppToolboxServer>(ctx);
}

const BackendSpec* spec() {
    static const BackendSpec kSpec(descriptor.recipe, descriptor.binary, nullptr, false);
    return &kSpec;
}

const BackendOps* ops() {
    static const ToolboxOps kOps(descriptor.recipe);
    return &kOps;
}

}  // namespace toolbox

}  // namespace backends
}  // namespace lemon
