#include "lemon/backends/halogen/halogen_server.h"
#include "lemon/backends/halogen/halogen.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <lemon/utils/aixlog.hpp>
#include "lemon/backends/backend_registry.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backends/container_backend.h"
#include "lemon/model_manager.h"
#include "lemon/utils/http_client.h"
#include "lemon/utils/process_manager.h"

#ifndef _WIN32
#include <sys/utsname.h>
#endif

namespace fs = std::filesystem;
using namespace lemon::utils;

namespace lemon {
namespace backends {

namespace {

constexpr const char* kVariant = "rocm";
constexpr int kNativeContext = 262144;

bool has_suffix(const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

}  // namespace

namespace halogen {

int running_kernel_major() {
#ifdef _WIN32
    return 0;
#else
    struct utsname info;
    if (uname(&info) != 0) return 0;
    return std::atoi(info.release);
#endif
}

bool kernel_supports_halogen() {
    const int major = running_kernel_major();
    return major == 0 || major >= kMinKernelMajor;  // unknown kernel is not a hard block
}

}  // namespace halogen

BackendOps::InstallCheck HalogenOps::check_install(const std::string& backend,
                                                   bool binary_found) const {
    if (!halogen::kernel_supports_halogen()) {
        return {false, "Halogen needs Linux kernel " +
                           std::to_string(halogen::kMinKernelMajor) + ".0 or newer; this host runs " +
                           std::to_string(halogen::running_kernel_major()) + ".x"};
    }
    return ContainerBackendOps::check_install(backend, binary_found);
}

std::optional<BackendOps::UnavailableState> HalogenOps::classify_unavailable(
    const std::string& backend, const std::string& install_error,
    const std::string& default_install_command) const {
    if (!halogen::kernel_supports_halogen()) {
        UnavailableState state;
        state.state = "action_required";
        state.message = install_error;
        state.action = prerequisites_url("halogen-kernel");
        return state;
    }
    return ContainerBackendOps::classify_unavailable(backend, install_error,
                                                     default_install_command);
}

std::optional<std::vector<std::string>> HalogenOps::select_checkpoint_files(
    const std::string& main_variant, const std::vector<std::string>& repo_files) const {
    if (!has_suffix(main_variant, ".hgn")) {
        return std::nullopt;
    }
    // An HGN bundle is the base checkpoint plus its overlays, the optional
    // vision tower and the tokenizer directory. Every registered Halogen model
    // names the same base checkpoint and differs only in which overlay it
    // selects at launch, so one download serves all of them.
    std::vector<std::string> selected;
    for (const auto& file : repo_files) {
        if (has_suffix(file, ".hgn") || file.rfind("tokenizer/", 0) == 0) {
            selected.push_back(file);
        }
    }
    if (std::find(selected.begin(), selected.end(), main_variant) == selected.end()) {
        throw std::runtime_error("Halogen checkpoint not found in repository: " + main_variant);
    }
    return selected;
}

HalogenServer::HalogenServer(const std::string& log_level, ModelManager* model_manager,
                             BackendManager* backend_manager)
    : WrappedServer("halogen", log_level, model_manager, backend_manager) {}

HalogenServer::~HalogenServer() {
    unload();
}

void HalogenServer::load(const std::string& model_name, const ModelInfo& model_info,
                         const RecipeOptions& options, bool do_not_upgrade) {
    (void)do_not_upgrade;  // install_backend() is a no-op once the pinned digest is present

    if (!halogen::kernel_supports_halogen()) {
        throw std::runtime_error("Halogen needs Linux kernel " +
                                 std::to_string(halogen::kMinKernelMajor) +
                                 ".0 or newer. See " + prerequisites_url("halogen-kernel"));
    }

    const std::string checkpoint_path = model_info.resolved_path("main");
    if (checkpoint_path.empty() || !fs::exists(checkpoint_path)) {
        throw std::runtime_error("halogen: HGN checkpoint not found for model '" + model_name +
                                 "' (checkpoint: " + model_info.checkpoint() + ")");
    }

    // The overlay, the optional vision tower and the tokenizer all live beside
    // the checkpoint in the same snapshot directory.
    const fs::path bundle_dir = fs::path(checkpoint_path).parent_path();
    const std::string overlay = model_info.extra<std::string>("halogen_overlay", "");
    const std::string tokenizer_dir = model_info.extra<std::string>("halogen_tokenizer",
                                                                    "tokenizer");
    const std::string vision_tower = model_info.extra<std::string>("halogen_vision_tower", "");

    if (overlay.empty()) {
        throw std::runtime_error("halogen: model '" + model_name +
                                 "' declares no halogen_overlay");
    }
    for (const auto& required : {overlay, tokenizer_dir}) {
        if (!fs::exists(bundle_dir / required)) {
            throw std::runtime_error("halogen: bundle is incomplete, missing " + required +
                                     " in " + bundle_dir.string());
        }
    }
    if (!vision_tower.empty() && !fs::exists(bundle_dir / vision_tower)) {
        throw std::runtime_error("halogen: bundle is incomplete, missing vision tower " +
                                 vision_tower + " in " + bundle_dir.string());
    }

    int ctx_size = options.get_option("ctx_size");
    if (ctx_size <= 0 || ctx_size > kNativeContext) {
        ctx_size = kNativeContext;
    }

    device_type_ = DEVICE_GPU;
    backend_manager_->install_backend(halogen::descriptor.recipe, kVariant);

    port_ = choose_port();
    clear_stale_container(halogen::descriptor.recipe, kVariant);

    ContainerLaunchRequest request;
    request.recipe = halogen::descriptor.recipe;
    request.variant = kVariant;
    request.profile_id = "halogen-strix-halo";
    request.extra_mounts.push_back(bundle_dir.string());
    ContainerLaunchPlan plan = plan_container_launch(request, port_);

    const std::string bundle_in_container = plan.container_path(bundle_dir.string());
    plan.set_command({});  // the image's entrypoint takes no argv

    plan.add_env("HALOGEN_CHECKPOINT", plan.container_path(checkpoint_path));
    plan.add_env("HALOGEN_CK_OVERLAY", bundle_in_container + "/" + overlay);
    plan.add_env("HALOGEN_TOKENIZER", bundle_in_container + "/" + tokenizer_dir);
    plan.add_env("HALOGEN_API_PORT", std::to_string(port_));
    plan.add_env("HALOGEN_CTX", std::to_string(ctx_size));
    // HALOGEN_KV_POOL_POSITIONS is deliberately left unset: the engine sizes the
    // pool from the memory the OS reports and lowers it when the configured one
    // will not fit. A host that carves a large block out for the iGPU in
    // firmware leaves it too little to work with, and the answer there is the
    // firmware setting, not a smaller pool - see the prerequisites page.
    plan.add_env("HALOGEN_PROMPT_CACHE", "2");
    if (!vision_tower.empty()) {
        plan.add_env("HALOGEN_VISION_TOWER", bundle_in_container + "/" + vision_tower);
    }

    const std::vector<std::string> engine_args = plan.engine_args();

    LOG(INFO, "Halogen") << "Starting " << plan.image().tagged_ref() << " as "
                         << plan.container_name() << " for " << model_name << " on port " << port_
                         << std::endl;

    const bool inherit_output = (log_level_ == "info") || is_debug();
    set_process_handle(ProcessManager::start_process(plan.engine_executable(), engine_args, "",
                                                     inherit_output, true, {}),
                       plan.engine_executable(), engine_args);

    // Halogen maps a 115 GiB checkpoint before it binds, so readiness takes far
    // longer than a GGUF load; /v1/models is the cheapest always-on route.
    if (!wait_for_ready("/v1/models", HttpClient::get_default_timeout())) {
        unload();
        throw std::runtime_error("Halogen server failed to start within timeout");
    }
}

void HalogenServer::unload() {
    stop_backend_watchdog();

    stop_container_for(halogen::descriptor.recipe, kVariant);

    const ProcessHandle handle = consume_process_handle_for_cleanup();
    if (has_process_handle(handle)) {
        LOG(INFO, "Halogen") << "Stopping Halogen server" << std::endl;
        ProcessManager::stop_process(handle);
    }
}

json HalogenServer::chat_completion(const json& request) {
    return forward_request("/v1/chat/completions", request);
}

json HalogenServer::completion(const json& request) {
    return forward_request("/v1/completions", request);
}

namespace halogen {

std::unique_ptr<WrappedServer> create(const BackendContext& ctx) {
    return make_server<HalogenServer>(ctx);
}

const BackendSpec* spec() {
    static const BackendSpec kSpec(descriptor.recipe, descriptor.binary, nullptr, false);
    return &kSpec;
}

const BackendOps* ops() {
    return single_ops<HalogenOps>();
}

}  // namespace halogen

}  // namespace backends
}  // namespace lemon
