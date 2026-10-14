#include "lemon/backends/backend_load_helpers.h"
#include "lemon/backends/backend_ops.h"
#include "lemon/backends/backend_registry.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backends/hf_cache_util.h"
#include "lemon/backends/thinksound/thinksound.h"
#include "lemon/backends/thinksound/thinksound_server.h"
#include "lemon/error_types.h"

#include <algorithm>
#include <filesystem>
#include <lemon/utils/aixlog.hpp>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace lemon {
namespace backends {

InstallParams ThinkSoundServer::get_install_params(const std::string& backend, const std::string& version) {
    (void)version;
    InstallParams params;
    params.repo = "pwilkin/thinksound.cpp";
    const std::string variant = (backend.rfind("rocm", 0) == 0) ? "rocm" : backend;
#ifdef _WIN32
    params.filename = "thinksound-" + variant + "-windows-x64.zip";
#else
    params.filename = "thinksound-" + variant + "-linux-x64.tar.gz";
#endif
    return params;
}

ThinkSoundServer::ThinkSoundServer(const std::string& log_level,
                                   ModelManager* model_manager,
                                   BackendManager* backend_manager)
    : ThinkSoundServer(log_level, model_manager, backend_manager,
                       production_backend_effects(backend_manager, [this] { return choose_port(); })) {}

ThinkSoundServer::ThinkSoundServer(const std::string& log_level, ModelManager* model_manager,
                                 BackendManager* backend_manager, BackendEffects effects)
    : WrappedServer("thinksound-server", log_level, model_manager, backend_manager),
      effects_(std::move(effects)) {
    bind_legacy_operations(LegacyOperationProfile::ThinkSound, *this);
}

void ThinkSoundServer::load(const std::string& model_name,
                            const ModelInfo& model_info,
                            const RecipeOptions& options,
                            bool /*do_not_upgrade*/) {
    LOG(INFO, "thinksound-server") << "Loading model: " << model_name << std::endl;

    const std::string model_path = model_info.resolved_path();
    if (model_path.empty() || !effects_.exists(model_path)) {
        throw std::runtime_error("Model path not found for checkpoint: " + model_info.checkpoint());
    }

    std::string backend = options.get_option("thinksound_backend");
    if (backend.empty()) {
        auto supported = effects_.supported_backends("thinksound");
        backend = first_supported_backend(
            supported, "ThinkSound audio generation", "this system: no supported GPU backend (Vulkan, ROCm, or CUDA) detected");
    }
    effects_.validate_backend("thinksound", backend);
    const std::string exe_path = resolve_external_or_managed_binary(
        *thinksound::spec(), backend, effects_.binary);

    port_ = effects_.allocate_port();
    if (port_ == 0) {
        throw std::runtime_error("Failed to find an available port");
    }

    // The checkpoint is the directory of ThinkSound GGUFs; ts-server resolves the
    // individual networks (dit/t5/clip/vae/tokenizers) within it.
    std::vector<std::string> args = {
        "--dir", model_path,
        "--host", "127.0.0.1",
        "--port", std::to_string(port_),
    };

    // ROCm/CUDA: the slim binary finds its runtime in the shared TheRock SDK
    // (ROCm) or bundled next to the exe (CUDA), plus the colocated ggml library.
    // Prepend those dirs to the loader path (mirrors sd-cpp / llama.cpp). Vulkan
    // needs no special env.
    std::vector<std::pair<std::string, std::string>> env_vars;
    const std::string exe_dir = std::filesystem::path(exe_path).parent_path().string();
    auto prepend_loader_path = [this, &env_vars, &exe_dir](const std::string& extra_dirs) {
#ifdef _WIN32
        constexpr bool windows = true;
#else
        constexpr bool windows = false;
#endif
        env_vars.push_back(make_loader_path_override(
            exe_dir, extra_dirs, effects_.inherited_loader(), windows));
    };
    if (backend == "rocm") {
        const std::string arch = effects_.rocm_arch();
        std::string dirs;
        if (!arch.empty()) {
            dirs = effects_.runtime_directories(arch);
        }
        prepend_loader_path(dirs);
    } else if (backend == "cuda") {
        prepend_loader_path("");
        effects_.append_cuda(env_vars, "thinksound-server");
    }

    LOG(INFO, "thinksound-server") << "Starting " << exe_path << " on port " << port_ << std::endl;
    ServerCommand command;
    command.program = exe_path;
    command.args = std::move(args);
    command.env = std::move(env_vars);
    command.port = port_;
    start_server(effects_.make_process(ProcessOutput{is_debug(), false}, ""), command);
}

void ThinkSoundServer::audio_generations(const json& request, httplib::DataSink& sink) {
    // Map the Lemonade /audio/generations request onto ts-server's /generate, then
    // stream the wav bytes back. On failure the sink receives ts-server's JSON
    // error body (or nothing) — the endpoint handler maps both to an HTTP error.
    json body;
    body["caption"] = request.value("prompt", std::string());
    body["description"] = request.contains("description") ? request["description"]
                        : request.contains("cot") ? request["cot"]
                        : body["caption"];
    if (request.contains("duration")) body["duration"] = request["duration"];
    if (request.contains("steps"))    body["steps"]    = request["steps"];
    if (request.contains("cfg"))      body["cfg"]      = request["cfg"];
    if (request.contains("seed"))     body["seed"]     = request["seed"];
    forward_streaming_request("/generate", body.dump(), sink, /*sse=*/false, /*timeout_seconds=*/600);
}

}  // namespace backends

namespace backends {

namespace {
// The checkpoint is a directory of ThinkSound GGUFs; resolve to the active
// Hugging Face snapshot directory (ts-server scans it via --dir).
class ThinkSoundOps : public BackendOps {
public:
    // The repo also carries q8 variants of the dit/t5 networks, but ts-server
    // loads the bf16 set by default; skip the redundant ~5 GB.
    std::optional<std::vector<std::string>> select_checkpoint_files(
        const std::string& main_variant, const std::vector<std::string>& repo_files) const override {
        (void)main_variant;
        static const std::vector<std::string> kWanted = {
            "dit-bf16.gguf",
            "t5-bf16.gguf",
            "metaclip-text-f32.gguf",
            "vae-f32.gguf",
            "t5-tokenizer.gguf",
            "clip-tokenizer.gguf",
        };
        std::vector<std::string> want;
        for (const auto& f : kWanted) {
            if (std::find(repo_files.begin(), repo_files.end(), f) != repo_files.end()) {
                want.push_back(f);
            }
        }
        return want;
    }

    std::string resolve_checkpoint_path(const ModelInfo& info,
                                        const CheckpointResolveContext& ctx) const override {
        (void)info;
        return hf_cache::active_snapshot_or_cache_path(ctx.model_cache_path);
    }
};
}  // namespace

namespace thinksound {

std::unique_ptr<WrappedServer> create(const BackendContext& ctx) {
    return make_server<ThinkSoundServer>(ctx);
}

const BackendSpec* spec() { return make_spec<ThinkSoundServer>(descriptor); }
const BackendOps* ops() { return single_ops<ThinkSoundOps>(); }

}  // namespace thinksound
}  // namespace backends
}  // namespace lemon
