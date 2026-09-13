#include "lemon/backends/ds4/ds4_server.h"
#include "lemon/backends/ds4/ds4.h"
#include "lemon/backends/backend_registry.h"
#include "lemon/backends/backend_ops.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backends/container_backend.h"
#include "lemon/model_manager.h"
#include "lemon/system_info.h"
#include "lemon/utils/custom_args.h"
#include "lemon/utils/http_client.h"
#include "lemon/utils/process_manager.h"
#include <lemon/utils/aixlog.hpp>
#include <filesystem>
#include <set>

namespace fs = std::filesystem;
using namespace lemon::utils;

namespace lemon {
namespace backends {

namespace {
constexpr const char* kVariant = "rocm";
}

Ds4Server::Ds4Server(const std::string& log_level, ModelManager* model_manager,
                     BackendManager* backend_manager)
    : WrappedServer("ds4-server", log_level, model_manager, backend_manager) {
}

Ds4Server::~Ds4Server() {
    unload();
}

void Ds4Server::load(const std::string& model_name, const ModelInfo& model_info,
                     const RecipeOptions& options, bool do_not_upgrade) {
    (void)do_not_upgrade;  // install_backend() is a no-op once the pinned digest is present

    std::string ds4_args = options.get_option("ds4_args");
    int ctx_size = options.get_option("ctx_size");

    // ds4-server only runs its own curated GGUFs. Accept either a
    // Hugging-Face-resolved local path or an absolute path used directly as
    // the checkpoint (user_models.json registrations of an existing file).
    std::string gguf_path = model_info.resolved_path("main");
    if (gguf_path.empty() || !fs::exists(gguf_path)) {
        const std::string checkpoint = model_info.checkpoint();
        if (!checkpoint.empty() && fs::path(checkpoint).is_absolute() && fs::exists(checkpoint)) {
            gguf_path = checkpoint;
        }
    }
    if (gguf_path.empty() || !fs::exists(gguf_path)) {
        throw std::runtime_error("ds4: no local GGUF found for model '" + model_name +
                                 "' (checkpoint: " + model_info.checkpoint() + ")");
    }

    device_type_ = DEVICE_GPU;
    backend_manager_->install_backend(ds4::descriptor.recipe, kVariant);

    port_ = choose_port();
    clear_stale_container(ds4::descriptor.recipe, kVariant);

    ContainerLaunchRequest request;
    request.recipe = ds4::descriptor.recipe;
    request.variant = kVariant;
    request.profile_id = "ds4-rocm";
    // A model registered by absolute path can live outside the HF cache, so
    // mount its directory too.
    request.extra_mounts.push_back(fs::path(gguf_path).parent_path().string());
    ContainerLaunchPlan plan = plan_container_launch(request, port_);

    std::vector<std::string> command;
    command.push_back("ds4-server");
    command.push_back("-m");
    command.push_back(plan.container_path(gguf_path));
    command.push_back("--host");
    command.push_back("0.0.0.0");  // reachable from the published loopback port only
    command.push_back("--port");
    command.push_back(std::to_string(port_));
    if (ctx_size > 0) {
        command.push_back("--ctx");
        command.push_back(std::to_string(ctx_size));
    }

    // ds4-server defaults to full residency, which maps the entire model into
    // the ROCm arena. The published ds4 models are 80 GB+ DeepSeek V4 MoEs, and
    // the supported devices top out around a 64 GB VRAM carveout with a smaller
    // usable arena, so full residency always OOMs mid-load. Stream experts from
    // disk instead; a user-supplied later flag (e.g.
    // --ssd-streaming-cache-experts) still wins since ds4-server parses
    // left-to-right.
    command.push_back("--ssd-streaming");

    if (!ds4_args.empty()) {
        const std::string validation_error =
            validate_custom_args(ds4_args, ds4::reserved_custom_arg_flags());
        if (!validation_error.empty()) {
            throw std::invalid_argument("Invalid custom ds4-server arguments:\n" + validation_error);
        }
        LOG(DEBUG, "DS4") << "Adding custom arguments: " << ds4_args << std::endl;
        std::vector<std::string> custom_args = parse_custom_args(ds4_args);
        command.insert(command.end(), custom_args.begin(), custom_args.end());
    }

    plan.set_command(std::move(command));
    const std::vector<std::string> engine_args = plan.engine_args();

    LOG(INFO, "DS4") << "Starting " << plan.image().tagged_ref() << " as "
                     << plan.container_name() << " for " << gguf_path << " on port " << port_
                     << std::endl;

    const bool inherit_output = (log_level_ == "info") || (log_level_ == "debug");
    set_process_handle(ProcessManager::start_process(plan.engine_executable(), engine_args, "",
                                                     inherit_output, true, {}),
                       plan.engine_executable(), engine_args);

    // ds4-server binds its port only after the model is fully loaded, so first
    // reachability means ready. There is no /health endpoint; /v1/models is the
    // cheapest always-on route and doubles as the watchdog probe.
    if (!wait_for_ready("/v1/models", HttpClient::get_default_timeout())) {
        unload();
        throw std::runtime_error("ds4-server failed to start within timeout");
    }
}

void Ds4Server::unload() {
    stop_backend_watchdog();

    stop_container_for(ds4::descriptor.recipe, kVariant);

    const ProcessHandle handle = consume_process_handle_for_cleanup();
    if (has_process_handle(handle)) {
        LOG(INFO, "DS4") << "Stopping ds4-server" << std::endl;
        ProcessManager::stop_process(handle);
    }
}

json Ds4Server::chat_completion(const json& request) {
    return forward_request("/v1/chat/completions", request);
}

json Ds4Server::completion(const json& request) {
    return forward_request("/v1/completions", request);
}

json Ds4Server::responses(const json& request) {
    return forward_request("/v1/responses", request);
}

std::string Ds4Ops::remove_legacy_binary_install() {
    // Pre-container DS4 unpacked lemonade-sdk/ds4-rocm here.
    const std::string install_dir =
        BackendUtils::get_install_directory(ds4::descriptor.recipe, kVariant);
    std::error_code ec;
    if (!fs::exists(install_dir, ec)) {
        return "";
    }
    fs::remove_all(install_dir, ec);
    if (ec) {
        LOG(WARNING, "DS4") << "Could not remove the superseded ds4-server binary at "
                            << install_dir << ": " << ec.message() << std::endl;
        return "";
    }
    LOG(INFO, "DS4") << "Removed the superseded ds4-server binary install at " << install_dir
                     << "; DS4 now runs from a container image" << std::endl;
    return install_dir;
}

bool Ds4Ops::install(const std::string& backend, bool force,
                     DownloadProgressCallback progress) const {
    remove_legacy_binary_install();
    return ContainerBackendOps::install(backend, force, progress);
}

bool Ds4Ops::uninstall(const std::string& backend) const {
    remove_legacy_binary_install();
    return ContainerBackendOps::uninstall(backend);
}

namespace ds4 {

std::unique_ptr<WrappedServer> create(const BackendContext& ctx) {
    return make_server<Ds4Server>(ctx);
}

const BackendSpec* spec() {
    static const BackendSpec kSpec(descriptor.recipe, descriptor.binary, nullptr, false);
    return &kSpec;
}

const BackendOps* ops() {
    return single_ops<Ds4Ops>();
}

}  // namespace ds4

}  // namespace backends
}  // namespace lemon
