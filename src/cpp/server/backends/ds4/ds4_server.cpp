#include "lemon/backends/ds4/ds4_server.h"
#include "lemon/backends/ds4/ds4.h"
#include "lemon/backends/backend_registry.h"
#include "lemon/backends/backend_ops.h"
#include "lemon/backends/backend_utils.h"
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

// ds4-server sizes its expert cache from the whole device arena, which on an
// APU is the GTT window. That leaves a long prompt's prefill too little: its
// next expert span pushes free memory under ds4's own 16 GiB reserve, the arena
// refuses it and the request fails with "rocm prefill failed". Half the arena
// leaves the prefill its room.
constexpr double kExpertCacheFraction = 0.5;

// The larger of the integrated GPU's carve-out and its GTT window, in GiB, or
// 0 when there is no integrated AMD GPU.
double igpu_pool_gb() {
    try {
        const GPUInfo igpu = create_system_info()->get_amd_igpu_device();
        if (igpu.available) {
            return (std::max)(igpu.vram_gb, igpu.virtual_gb);
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
    (void)do_not_upgrade;  // install_backend() is a no-op once the pin is present

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

    // rocm is the only published variant, so there is no backend option to
    // read; ds4_args cannot select one either (see reserved_custom_arg_flags).
    const std::string backend = "rocm";
    device_type_ = DEVICE_GPU;

    backend_manager_->install_backend(ds4::spec()->recipe, backend);
    const std::string image = BackendUtils::get_backend_image(ds4::descriptor.recipe, backend);

    port_ = choose_port();

    std::vector<std::string> args;
    args.push_back("-m");
    args.push_back(gguf_path);
    args.push_back("--host");
    args.push_back("0.0.0.0");
    args.push_back("--port");
    args.push_back(std::to_string(port_));
    if (ctx_size > 0) {
        args.push_back("-c");
        args.push_back(std::to_string(ctx_size));
    }

    // ds4-server defaults to full residency, which maps the entire model into
    // the ROCm arena. The only published ds4 model is an 81 GB DeepSeek V4 MoE,
    // and the only supported device (gfx1151) tops out around a 64 GB VRAM
    // carveout with a smaller usable arena, so full residency always OOMs
    // mid-load. Stream experts from disk instead; a user-supplied later flag
    // (e.g. --ssd-streaming-cache-experts) still wins since ds4-server parses
    // left-to-right.
    args.push_back("--ssd-streaming");

    // A long prompt's default 4096-token prefill graph faults the GPU in a
    // quantize kernel and takes the server with it; chunked, it completes.
    args.push_back("--prefill-chunk");
    args.push_back("2048");
    const int expert_cache_gb = static_cast<int>(igpu_pool_gb() * kExpertCacheFraction);
    if (expert_cache_gb > 0) {
        args.push_back("--ssd-streaming-cache-experts");
        args.push_back(std::to_string(expert_cache_gb) + "GB");
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

    LOG(INFO, "DS4") << "Starting ds4-server (" << image << ") for " << gguf_path << " on port "
                     << port_ << std::endl;

    ServerCommand command;
    command.program = ds4::descriptor.binary;
    command.args = std::move(args);
    command.model_files = {gguf_path};
    command.port = port_;
    // ds4-server binds its port only after the model is fully loaded, so first
    // reachability means ready. There is no /health endpoint; /v1/models is the
    // cheapest always-on route and doubles as the watchdog probe.
    command.ready_endpoint = "/v1/models";

    const bool inherit_output = (log_level_ == "info") || (log_level_ == "debug");
    start_server(std::make_unique<ContainerProcess>(ProcessOutput{inherit_output, true},
                                                    ds4::descriptor.recipe, backend, model_name,
                                                    *ds4::descriptor.container_for(backend), image),
                 command, HttpClient::get_default_timeout());
}

void Ds4Server::unload() {
    stop_server();
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

namespace ds4 {

std::unique_ptr<WrappedServer> create(const BackendContext& ctx) {
    return make_server<Ds4Server>(ctx);
}

const BackendSpec* spec() {
    static const BackendSpec kSpec(descriptor.recipe, descriptor.binary);
    return &kSpec;
}

const BackendOps* ops() {
    return default_backend_ops();
}

}  // namespace ds4

}  // namespace backends
}  // namespace lemon
