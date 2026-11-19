#include "lemon/backends/aim/aim_server.h"

#include "lemon/utils/http_client.h"

namespace lemon {
namespace backends {

namespace {

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

void translate_vllm_reply(json& reply, const std::string& model_name) {
    restore_model_name(reply, model_name);
    if (!reply.is_object() || !reply.contains("choices") || !reply["choices"].is_array()) return;
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

AimVllmServer::AimVllmServer(const std::string& log_level, ModelManager* model_manager,
                             BackendManager* backend_manager)
    : VLLMServer("aim", log_level, model_manager, backend_manager) {}

void AimVllmServer::load(const std::string& model_name, const ModelInfo& model_info,
                         const RecipeOptions& options, bool do_not_upgrade) {
    (void)do_not_upgrade;  // install_backend() is a no-op once the pinned base is present

    // An automatic ctx_size is the router's estimate from the model's size,
    // so the profile's tuned context stands instead.
    json engine_args = json::object();
    const json ctx_option = options.get_option("ctx_size");
    if (!ctx_size_is_auto() && ctx_option.is_number_integer() && ctx_option.get<int>() > 0) {
        engine_args["max-model-len"] = ctx_option.get<int>();
    }

    device_type_ = DEVICE_GPU;
    port_ = choose_port();
    aim::Launch launch = aim::prepare_launch(model_name, model_info, options, model_manager_,
                                             backend_manager_, port_, engine_args,
                                             (log_level_ == "info") || is_debug());
    served_model_ = launch.served_model;
    start_server(std::move(launch.process), launch.command, launch.startup_timeout_seconds);
    max_model_len_ = served_max_model_len(get_base_url());
}

json AimVllmServer::to_aim_request(const json& request) const {
    json forwarded = request;
    forwarded["model"] = served_model_;
    return forwarded;
}

json AimVllmServer::chat_completion(const json& request) {
    json reply = VLLMServer::chat_completion(to_aim_request(request));
    aim::translate_vllm_reply(reply, get_model_name());
    return reply;
}

json AimVllmServer::completion(const json& request) {
    json reply = VLLMServer::completion(to_aim_request(request));
    aim::translate_vllm_reply(reply, get_model_name());
    return reply;
}

json AimVllmServer::responses(const json& request) {
    json reply = VLLMServer::responses(to_aim_request(request));
    aim::translate_vllm_reply(reply, get_model_name());
    return reply;
}

StreamingProxy::ChunkRewriter AimVllmServer::sse_chunk_rewriter() const {
    return [model_name = get_model_name()](json& chunk) {
        aim::translate_vllm_reply(chunk, model_name);
    };
}

void AimVllmServer::forward_streaming_request(const std::string& endpoint,
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

}  // namespace backends
}  // namespace lemon
