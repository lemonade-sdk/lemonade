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

constexpr const char* kBackend = "radeon";

// vLLM compiles its kernels on the first start of each model and GPU, which
// global_timeout does not describe, so it gets its own floor; a larger
// global_timeout still raises it.
constexpr long kStartupTimeoutSeconds = 1800;

// A model image is the pinned base plus a few layers of profiles; this pulls
// those layers when the base is already installed.
void pull_model_image(const std::string& image) {
    const auto& manager = utils::ContainerManager::global();
    if (manager.has_image(image)) return;
    if (auto* cfg = RuntimeConfig::global()) {
        if (cfg->offline() || cfg->no_fetch_executables()) {
            throw std::runtime_error("aim: " + image + " is not pulled, and " +
                                     (cfg->offline() ? "lemond is offline"
                                                     : "fetching executable artifacts is disabled"));
        }
    }
    manager.pull(image, nullptr);
}

// The context vLLM settled on, from ctx_size or the profile, or 0 when it
// does not say.
int64_t served_max_model_len(const std::string& base_url) {
    try {
        const auto response = utils::HttpClient::get(
            base_url + "/v1/models", {}, 10, utils::HttpSecurityPolicy::TrustedLoopback);
        if (response.status_code != 200) return 0;
        const json models = json::parse(response.body);
        for (const auto& model : models.value("data", json::array())) {
            if (model.contains("max_model_len") && model["max_model_len"].is_number_integer()) {
                return model["max_model_len"].get<int64_t>();
            }
        }
    } catch (const std::exception&) {
    }
    return 0;
}

}  // namespace

namespace aim {

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

std::vector<std::pair<std::string, std::string>> build_container_env(int port, int ctx_size,
                                                                     const std::string& profile) {
    std::vector<std::pair<std::string, std::string>> env = {
        {"AIM_CACHE_PATH", kContainerModelsDir},
        {"AIM_PORT", std::to_string(port)},
        // One local user waits on each request, so of a model's tuned
        // profiles, take the one tuned for latency.
        {"AIM_METRIC", "latency"},
        {"HF_HUB_OFFLINE", "1"},
    };
    if (ctx_size > 0) {
        env.push_back({"AIM_ENGINE_ARGS", json{{"max-model-len", ctx_size}}.dump()});
    }
    if (!profile.empty()) {
        env.push_back({"AIM_PROFILE_ID", profile});
    }
    return env;
}

void translate_reply(json& reply, const std::string& model_name) {
    if (!reply.is_object()) return;
    if (reply.contains("model")) reply["model"] = model_name;
    if (reply.contains("response") && reply["response"].is_object() &&
        reply["response"].contains("model")) {
        reply["response"]["model"] = model_name;
    }
    if (!reply.contains("choices") || !reply["choices"].is_array()) return;
    for (auto& choice : reply["choices"]) {
        for (const char* part : {"message", "delta"}) {
            if (!choice.contains(part) || !choice[part].is_object()) continue;
            json& body = choice[part];
            if (!body.contains("reasoning")) continue;
            json reasoning = std::move(body["reasoning"]);
            body.erase("reasoning");
            if (!body.contains("reasoning_content")) body["reasoning_content"] = std::move(reasoning);
        }
    }
}

}  // namespace aim

AimServer::AimServer(const std::string& log_level, ModelManager* model_manager,
                     BackendManager* backend_manager)
    : VLLMServer("aim", log_level, model_manager, backend_manager) {}

void AimServer::load(const std::string& model_name, const ModelInfo& model_info,
                     const RecipeOptions& options, bool do_not_upgrade) {
    (void)do_not_upgrade;  // install_backend() is a no-op once the pinned base is present
    backend_manager_->install_backend(aim::descriptor.recipe, kBackend);

    const std::string pinned = model_info.extra<std::string>("aim_image", "");
    if (pinned.empty()) {
        throw std::runtime_error("aim: model '" + model_name + "' names no aim_image");
    }
    const aim::ModelImage image = aim::parse_model_image(pinned);
    pull_model_image(image.image);

    // AIM looks for the model at AIM_CACHE_PATH/<org>/<model>, where <org>/<model>
    // is the id its profiles name, so the checkpoint must be the AIM's own repository.
    const std::string checkpoint = model_info.checkpoint();
    const std::string repo_id = checkpoint.substr(0, checkpoint.find(':'));
    if (repo_id.empty() || model_manager_ == nullptr) {
        throw std::runtime_error("aim: no checkpoint to serve for model '" + model_name + "'");
    }
    const fs::path repo_cache =
        utils::path_from_utf8(model_manager_->get_hf_cache_dir()) /
        hf_cache::repo_id_to_cache_dir_name(repo_id, model_info.registry_source);
    const fs::path snapshot = hf_cache::active_snapshot_path(repo_cache);
    if (snapshot.empty() || !fs::exists(snapshot)) {
        throw std::runtime_error("aim: " + repo_id + " is not downloaded for model '" +
                                 model_name + "'");
    }

    // An automatic ctx_size is the router's estimate from the model's size,
    // so the profile's tuned context stands instead.
    const json ctx_option = options.get_option("ctx_size");
    const int ctx_size =
        !ctx_size_is_auto() && ctx_option.is_number_integer() ? ctx_option.get<int>() : 0;
    const json profile_option = options.get_option("aim_profile");
    const std::string profile = profile_option.is_string() ? profile_option.get<std::string>() : "";

    device_type_ = DEVICE_GPU;
    port_ = choose_port();
    served_model_ = repo_id;

    ServerCommand command;
    command.program = "serve";
    command.model_files_at = {{utils::path_to_utf8(snapshot), repo_id}};
    command.env = aim::build_container_env(port_, ctx_size, profile);
    command.port = port_;
    command.ready_endpoint = "/health";

    LOG(INFO, "AIM") << "Starting " << model_name << " from " << image.image << " (" << image.tag
                     << ") on port " << port_ << std::endl;
    const bool inherit_output = (log_level_ == "info") || is_debug();
    start_server(std::make_unique<ContainerProcess>(
                     ProcessOutput{inherit_output, true}, aim::descriptor.recipe, kBackend,
                     model_name, *aim::descriptor.container_for(kBackend), image.image),
                 command,
                 (std::max)(kStartupTimeoutSeconds, utils::HttpClient::get_default_timeout()));
    max_model_len_ = served_max_model_len(get_base_url());
}

json AimServer::to_aim_request(const json& request) const {
    json forwarded = request;
    forwarded["model"] = served_model_;
    return forwarded;
}

json AimServer::chat_completion(const json& request) {
    json reply = VLLMServer::chat_completion(to_aim_request(request));
    aim::translate_reply(reply, get_model_name());
    return reply;
}

json AimServer::completion(const json& request) {
    json reply = VLLMServer::completion(to_aim_request(request));
    aim::translate_reply(reply, get_model_name());
    return reply;
}

json AimServer::responses(const json& request) {
    json reply = VLLMServer::responses(to_aim_request(request));
    aim::translate_reply(reply, get_model_name());
    return reply;
}

StreamingProxy::ChunkRewriter AimServer::sse_chunk_rewriter() const {
    return [model_name = get_model_name()](json& chunk) {
        aim::translate_reply(chunk, model_name);
    };
}

void AimServer::forward_streaming_request(const std::string& endpoint,
                                          const std::string& request_body,
                                          httplib::DataSink& sink, bool sse,
                                          long timeout_seconds,
                                          TelemetryCallback telemetry_callback) {
    std::string body = request_body;
    try {
        body = to_aim_request(json::parse(request_body)).dump();
    } catch (const json::exception&) {
        // vLLM reports the malformed body itself.
    }
    VLLMServer::forward_streaming_request(endpoint, body, sink, sse, timeout_seconds,
                                          telemetry_callback);
}

namespace aim {

std::unique_ptr<WrappedServer> create(const BackendContext& ctx) {
    return make_server<AimServer>(ctx);
}

const BackendSpec* spec() {
    static const BackendSpec kSpec(descriptor.recipe, descriptor.binary);
    return &kSpec;
}

const BackendOps* ops() {
    return default_backend_ops();
}

}  // namespace aim
}  // namespace backends
}  // namespace lemon
