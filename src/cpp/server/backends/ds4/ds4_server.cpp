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
#include <lemon/utils/aixlog.hpp>
#include <algorithm>
#include <filesystem>
#include <set>

namespace fs = std::filesystem;
using namespace lemon::utils;

namespace lemon {
namespace backends {

namespace {
constexpr const char* kVariant = "rocm";

// ds4-server streams an 80 GB+ MoE off disk and binds only once it is loaded,
// so readiness is bounded by storage bandwidth rather than by anything
// global_timeout describes. Floor the wait here and let a larger
// global_timeout raise it.
constexpr long kStartupTimeoutSeconds = 1800;

// ds4-server sizes its expert cache from the whole device arena, which on an
// APU is the GTT window. That leaves too little for a long prompt's prefill:
// the prefill asks for another multi-hundred-MiB expert span, the arena refuses
// because free has fallen under ds4's own 16 GiB reserve, and the request dies.
// Measured on a 61.3 GiB arena: ds4's own choice of a 40.9 GiB cache planned
// 48.2 GiB and faulted on a 2.6k-token prompt, while half the arena planned
// 37.4 GiB and answered it. Half is the default; --ds4-args overrides it.
constexpr double kExpertCacheFraction = 0.5;

// Largest memory pool the GPU can draw on, in GiB, or 0 when undetectable. An
// integrated GPU's carve-out and its GTT window are both views of system RAM,
// so the larger of the two is what it can actually address.
double gpu_pool_gb() {
    try {
        auto sys_info = create_system_info();
        const GPUInfo igpu = sys_info->get_amd_igpu_device();
        if (igpu.available) {
            return igpu.vram_gb > igpu.virtual_gb ? igpu.vram_gb : igpu.virtual_gb;
        }
    } catch (...) {
    }
    return 0.0;
}
}  // namespace

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

    std::vector<std::string> args;
    args.push_back("-m");
    args.push_back(gguf_path);
    args.push_back("--host");
    args.push_back("0.0.0.0");  // reachable from the published loopback port only
    args.push_back("--port");
    args.push_back(std::to_string(port_));
    if (ctx_size > 0) {
        args.push_back("--ctx");
        args.push_back(std::to_string(ctx_size));
    }

    // ds4-server defaults to full residency, which maps the entire model into
    // the ROCm arena. The published ds4 models are 80 GB+ DeepSeek V4 MoEs, and
    // the supported devices top out around a 64 GB VRAM carveout with a smaller
    // usable arena, so full residency always OOMs mid-load. Stream experts from
    // disk instead; a user-supplied later flag (e.g.
    // --ssd-streaming-cache-experts) still wins since ds4-server parses
    // left-to-right.
    args.push_back("--ssd-streaming");

    // Chunk the prefill graph. Without this a long prompt does not merely fail:
    // it faults the GPU (HSA_STATUS_ERROR_MEMORY_FAULT in a quantize kernel) and
    // takes the container with it, where chunked it returns a clean error.
    args.push_back("--prefill-chunk");
    args.push_back("2048");

    const double pool_gb = gpu_pool_gb();
    if (pool_gb > 0.0) {
        const int cache_gb = static_cast<int>(pool_gb * kExpertCacheFraction);
        if (cache_gb > 0) {
            // The GB form also reserves two full prefill layers, which is the
            // headroom the long-prompt path needs.
            args.push_back("--ssd-streaming-cache-experts");
            args.push_back(std::to_string(cache_gb) + "GB");
            LOG(DEBUG, "DS4") << "Capping the expert cache at " << cache_gb
                              << " GB of the " << pool_gb << " GB device pool" << std::endl;
        }
    }

    if (!ds4_args.empty()) {
        const std::string validation_error =
            validate_custom_args(ds4_args, ds4::reserved_custom_arg_flags());
        if (!validation_error.empty()) {
            throw std::invalid_argument("Invalid custom ds4-server arguments:\n" + validation_error);
        }
        LOG(DEBUG, "DS4") << "Adding custom arguments: " << ds4_args << std::endl;
        std::vector<std::string> custom_args = parse_custom_args(ds4_args);
        args.insert(args.end(), custom_args.begin(), custom_args.end());
    }

    ContainerTarget target;
    target.entry = "ds4-server";
    target.recipe = ds4::descriptor.recipe;
    target.variant = kVariant;
    target.profile_id = "ds4-rocm";
    target.model_paths.push_back(gguf_path);

    // ds4-server binds its port only after the model is fully loaded, so first
    // reachability means ready. There is no /health endpoint; /v1/models is the
    // cheapest always-on route and doubles as the watchdog probe.
    launch(std::move(args), target, "/v1/models",
           (std::max)(kStartupTimeoutSeconds, HttpClient::get_default_timeout()),
           (log_level_ == "info") || (log_level_ == "debug"));
}

void Ds4Server::unload() {
    stop_backend_watchdog();
    stop_child();
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
