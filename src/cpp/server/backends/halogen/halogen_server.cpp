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

#ifndef _WIN32
#include <sys/utsname.h>
#endif

namespace fs = std::filesystem;
using namespace lemon::utils;

namespace lemon {
namespace backends {

namespace {

constexpr const char* kBackend = "rocm";
constexpr int kNativeContext = 262144;
// The image's own built-in chat budget, which it applies when nothing overrides it.
constexpr int kDefaultMaxTokens = 8192;

// Pinning ~68 GiB of weights off disk is the bulk of a cold start, so readiness
// is bounded by storage bandwidth rather than by anything global_timeout
// describes. Floor the wait here and let a larger global_timeout raise it.
constexpr long kStartupTimeoutSeconds = 1800;

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

void HalogenOps::populate_metadata(ModelInfo& info, const BackendOpsContext& ctx) const {
    (void)ctx;
    info.max_context_window = kNativeContext;
}

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
        state.action = utils::container_prerequisites_url("halogen-kernel");
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
                                 ".0 or newer. See " +
                                 utils::container_prerequisites_url("halogen-kernel"));
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

    // Auto-tune sizes a context from GGUF architecture metadata against the GPU
    // pool. An HGN checkpoint carries no such metadata and Halogen keeps its KV
    // in host RAM, so that estimate describes neither the model nor the device.
    // Left alone the engine defaults to its native context and fits the KV pool
    // downward against the memory it measures, which is the better answer.
    const json ctx_json = options.get_option("ctx_size");
    int ctx_size = (!ctx_size_is_auto() && ctx_json.is_number()) ? ctx_json.get<int>() : 0;
    if (ctx_size > kNativeContext) {
        ctx_size = kNativeContext;
    }

    device_type_ = DEVICE_GPU;
    backend_manager_->install_backend(halogen::descriptor.recipe, kBackend);

    port_ = choose_port();

    const std::string overlay_path = (bundle_dir / overlay).string();
    const std::string tokenizer_path = (bundle_dir / tokenizer_dir).string();
    const std::string vision_tower_path =
        vision_tower.empty() ? std::string() : (bundle_dir / vision_tower).string();

    ServerCommand command;
    command.model_files = {checkpoint_path, overlay_path, tokenizer_path, vision_tower_path};
    command.env = {
        {"HALOGEN_CHECKPOINT", checkpoint_path},
        {"HALOGEN_CK_OVERLAY", overlay_path},
        {"HALOGEN_TOKENIZER", tokenizer_path},
        {"HALOGEN_API_PORT", std::to_string(port_)},
    };
    if (ctx_size > 0) {
        command.env.push_back({"HALOGEN_CTX", std::to_string(ctx_size)});
        // A request reserves prompt + max_tokens against the context, and the
        // image's built-in chat budget is 8192. Narrowing the context without
        // narrowing that budget rejects every request that omits max_tokens,
        // which is most OpenAI clients.
        command.env.push_back({"HALOGEN_MAX_TOKENS_DEFAULT",
                               std::to_string((std::min)(ctx_size / 2, kDefaultMaxTokens))});
    }
    // HALOGEN_KV_POOL_POSITIONS is deliberately left unset: the engine sizes the
    // pool from the memory the OS reports and lowers it when the configured one
    // will not fit. A host that carves a large block out for the iGPU in
    // firmware leaves it too little to work with, and the answer there is the
    // firmware setting, not a smaller pool - see the prerequisites page.
    command.env.push_back({"HALOGEN_PROMPT_CACHE", "2"});
    if (!vision_tower_path.empty()) {
        command.env.push_back({"HALOGEN_VISION_TOWER", vision_tower_path});
    }
    // Halogen maps a 115 GiB checkpoint before it binds, so readiness takes far
    // longer than a GGUF load; /v1/models is the cheapest always-on route.
    command.ready_endpoint = "/v1/models";

    LOG(INFO, "Halogen") << "Starting " << model_name << " on port " << port_ << std::endl;

    start_server(
        std::make_unique<ContainerProcess>(halogen::descriptor.recipe, kBackend, model_name),
        command, (log_level_ == "info") || is_debug(),
        (std::max)(kStartupTimeoutSeconds, HttpClient::get_default_timeout()));
}

void HalogenServer::unload() {
    stop_server();
}

json HalogenServer::chat_completion(const json& request) {
    return forward_request("/v1/chat/completions", request);
}

json HalogenServer::completion(const json& request) {
    return forward_request("/v1/completions", request);
}

json HalogenServer::responses(const json& request) {
    return forward_request("/v1/responses", request);
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
