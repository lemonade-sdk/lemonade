#include "lemon/backends/aim/aim_server.h"

#include "lemon/backend_manager.h"
#include "lemon/backends/aim/aim.h"
#include "lemon/backends/backend_ops.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backends/hf_cache_util.h"
#include "lemon/model_manager.h"
#include "lemon/runtime_config.h"
#include "lemon/utils/container_manager.h"
#include "lemon/utils/http_client.h"
#include "lemon/utils/path_utils.h"

#include <lemon/utils/aixlog.hpp>

#include <algorithm>
#include <filesystem>
#include <stdexcept>

namespace fs = std::filesystem;

namespace lemon {
namespace backends {

namespace {

constexpr const char* kBackend = "rocm";

// vLLM in AIM compiles its kernels on every start, since the container's caches
// go with it (about 2.5 minutes of a 3.5-minute load on an R9700). global_timeout
// does not describe that, so AIM gets its own floor; a larger one still raises it.
constexpr long kStartupTimeoutSeconds = 1800;

// A model image is the pinned base plus a few layers of profiles; this pulls
// those layers when the base is already installed. Recorded so that
// uninstalling the backend removes it, since it holds the base's layers too.
void pull_model_image(const std::string& image, const DownloadProgressCallback& progress) {
    const auto& manager = utils::ContainerManager::global();
    if (manager.has_image(image)) {
        BackendUtils::record_backend_extra_image(aim::descriptor.recipe, kBackend, image);
        return;
    }
    if (auto* cfg = RuntimeConfig::global()) {
        if (cfg->offline() || cfg->no_fetch_executables()) {
            throw std::runtime_error("aim: " + image + " is not pulled, and " +
                                     (cfg->offline() ? "lemond is offline"
                                                     : "fetching executable artifacts is disabled"));
        }
    }
    manager.pull(image, progress);
    BackendUtils::record_backend_extra_image(aim::descriptor.recipe, kBackend, image);
}

class AimOps : public BackendOps {
public:
    // The model's image comes down with its weights, so a pulled model loads
    // without the network.
    void download_model(const ModelInfo& info, bool do_not_upgrade,
                        DownloadProgressCallback progress,
                        const BackendOpsContext& ctx) const override {
        const std::string pinned = info.extra<std::string>("aim_image", "");
        if (!pinned.empty()) {
            // The weights' download reports completion; the image's must not.
            pull_model_image(aim::parse_model_image(pinned).image,
                             [&progress](const DownloadProgress& p) {
                                 if (!progress) return true;
                                 DownloadProgress partial = p;
                                 partial.complete = false;
                                 return progress(partial);
                             });
        }
        BackendOps::download_model(info, do_not_upgrade, std::move(progress), ctx);
    }
};

}  // namespace

namespace aim {

std::string model_engine(const ModelInfo& info) {
    return info.extra<std::string>("aim_engine", "");
}

bool supports_engine(const std::string& engine) {
    return engine == kEngineVllm;
}

ModelImage parse_model_image(const std::string& pinned) {
    const auto at = pinned.find('@');
    const auto colon = at == std::string::npos ? std::string::npos : pinned.rfind(':', at);
    const auto slash = pinned.rfind('/', at);
    if (colon == std::string::npos || (slash != std::string::npos && colon < slash)) {
        throw std::invalid_argument("aim_image '" + pinned +
                                    "' is not <repository>:<tag>@sha256:<digest>");
    }
    const std::string repository = pinned.substr(0, colon);
    const auto pin = parse_container_pin(pinned.substr(colon + 1));
    if (!pin) {
        throw std::invalid_argument("aim_image '" + pinned +
                                    "' is not <repository>:<tag>@sha256:<digest>");
    }
    if (!utils::ContainerManager::allowed_image(repository)) {
        throw std::invalid_argument("aim_image '" + pinned +
                                    "' is not an image Lemonade runs container backends from");
    }
    return {repository + "@" + pin->digest, pin->tag};
}

std::vector<std::pair<std::string, std::string>> build_container_env(int port,
                                                                     const json& engine_args,
                                                                     const std::string& profile) {
    std::vector<std::pair<std::string, std::string>> env = {
        {"AIM_CACHE_PATH", kContainerModelsDir},
        {"AIM_PORT", std::to_string(port)},
        // One local user waits on each request, so of a model's tuned
        // profiles, take the one tuned for latency.
        {"AIM_METRIC", "latency"},
        {"HF_HUB_OFFLINE", "1"},
    };
    if (!engine_args.empty()) {
        env.push_back({"AIM_ENGINE_ARGS", engine_args.dump()});
    }
    if (!profile.empty()) {
        env.push_back({"AIM_PROFILE_ID", profile});
    }
    return env;
}

void restore_model_name(json& reply, const std::string& model_name) {
    if (!reply.is_object()) return;
    if (reply.contains("model")) reply["model"] = model_name;
    if (reply.contains("response") && reply["response"].is_object() &&
        reply["response"].contains("model")) {
        reply["response"]["model"] = model_name;
    }
}

Launch prepare_launch(const std::string& model_name, const ModelInfo& model_info,
                      const RecipeOptions& options, ModelManager* model_manager,
                      BackendManager* backend_manager, int port, const json& engine_args,
                      bool inherit_output) {
    backend_manager->install_backend(descriptor.recipe, kBackend);

    const std::string pinned = model_info.extra<std::string>("aim_image", "");
    if (pinned.empty()) {
        throw std::runtime_error("aim: model '" + model_name + "' names no aim_image");
    }
    const ModelImage image = parse_model_image(pinned);
    pull_model_image(image.image, nullptr);

    // AIM looks for the model at AIM_CACHE_PATH/<org>/<model>, where <org>/<model>
    // is the id its profiles name, so the checkpoint must be the AIM's own repository.
    const std::string checkpoint = model_info.checkpoint();
    const std::string repo_id = checkpoint.substr(0, checkpoint.find(':'));
    if (repo_id.empty() || model_manager == nullptr) {
        throw std::runtime_error("aim: no checkpoint to serve for model '" + model_name + "'");
    }
    const fs::path repo_cache =
        utils::path_from_utf8(model_manager->get_hf_cache_dir()) /
        hf_cache::repo_id_to_cache_dir_name(repo_id, model_info.registry_source);
    const fs::path snapshot = hf_cache::active_snapshot_path(repo_cache);
    if (snapshot.empty() || !fs::exists(snapshot)) {
        throw std::runtime_error("aim: " + repo_id + " is not downloaded for model '" +
                                 model_name + "'");
    }

    const json profile_option = options.get_option("aim_profile");
    const std::string profile = profile_option.is_string() ? profile_option.get<std::string>() : "";

    Launch launch;
    launch.served_model = repo_id;
    launch.command.program = "serve";
    launch.command.model_files_at = {{utils::path_to_utf8(snapshot), repo_id}};
    launch.command.env = build_container_env(port, engine_args, profile);
    launch.command.port = port;
    launch.command.ready_endpoint = "/health";
    launch.process = std::make_unique<ContainerProcess>(
        ProcessOutput{inherit_output, true}, descriptor.recipe, kBackend, model_name,
        *descriptor.container_for(kBackend), image.image);
    launch.startup_timeout_seconds =
        (std::max)(kStartupTimeoutSeconds, utils::HttpClient::get_default_timeout());

    LOG(INFO, "AIM") << "Starting " << model_name << " (" << model_engine(model_info)
                     << ") from " << image.image << " (" << image.tag << ") on port " << port
                     << std::endl;
    return launch;
}

std::unique_ptr<WrappedServer> create(const BackendContext& ctx) {
    const std::string engine = ctx.model_info != nullptr ? model_engine(*ctx.model_info) : "";
    if (engine == kEngineVllm) return make_server<AimVllmServer>(ctx);
    if (engine.empty()) throw std::runtime_error("aim: model names no aim_engine");
    throw std::runtime_error("aim: model names aim_engine '" + engine +
                             "', which Lemonade does not run");
}

const BackendSpec* spec() {
    static const BackendSpec kSpec(descriptor.recipe, descriptor.binary);
    return &kSpec;
}

const BackendOps* ops() {
    return single_ops<AimOps>();
}

}  // namespace aim
}  // namespace backends
}  // namespace lemon
