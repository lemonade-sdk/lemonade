#pragma once

#include "../wrapped_server.h"
#include <string>

namespace lemon {
namespace backends {

/**
 * CloudServer offloads inference to a remote OpenAI-compatible cloud provider
 * (e.g., Fireworks AI) instead of running a local subprocess. It is selected
 * by the "cloud" recipe and the per-model "cloud_provider" field in
 * server_models.json.
 *
 * Unlike other WrappedServer subclasses, there is no managed subprocess and
 * no local port. load() validates that an API key is configured for the
 * provider, unload() is a no-op, and request methods rewrite the model id to
 * the upstream checkpoint and forward the request to the provider's HTTPS
 * endpoint with a Bearer auth header.
 */
class CloudServer : public WrappedServer, public IEmbeddingsServer {
public:
    CloudServer(const std::string& log_level,
                ModelManager* model_manager,
                BackendManager* backend_manager);

    ~CloudServer() override;

    void load(const std::string& model_name,
              const ModelInfo& model_info,
              const RecipeOptions& options,
              bool do_not_upgrade = false) override;

    void unload() override;

    json chat_completion(const json& request) override;
    json completion(const json& request) override;
    json responses(const json& request) override;

    json embeddings(const json& request) override;

    void forward_streaming_request(const std::string& endpoint,
                                   const std::string& request_body,
                                   httplib::DataSink& sink,
                                   bool sse = true,
                                   long timeout_seconds = 0) override;

private:
    json post_with_auth(const std::string& path, const json& request, long timeout_seconds = 0);
    json rewrite_model_field(const json& request) const;

    std::string provider_;       // e.g., "fireworks"
    std::string base_url_;       // e.g., "https://api.fireworks.ai/inference/v1"
    std::string api_key_;        // resolved at load time
    std::string upstream_model_; // provider's model id (from ModelInfo.checkpoint())
    bool loaded_ = false;
};

} // namespace backends
} // namespace lemon
