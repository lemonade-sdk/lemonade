#pragma once

#include "../model_manager.h"
#include "../wrapped_server.h"
#include <string>
#include <vector>

namespace lemon {
namespace backends {

/**
 * CloudServer offloads inference to a remote OpenAI-compatible cloud provider
 * (Fireworks, OpenAI, Together, Groq, OpenRouter, DeepInfra, vLLM, LM Studio,
 * etc.) instead of running a local subprocess. It is generic: the provider
 * name and base URL come from `cloud_offload.providers.<provider>` in
 * config.json, with no provider-specific code paths.
 *
 * Scope: chat-only (chat/completions and completions on OpenAI v1). Other
 * modalities — embeddings, audio, reranking, image — are intentionally not
 * served. discover_models() filters its result to LLM ids so the router
 * never sees a cloud model it cannot dispatch. Adding a modality means
 * adding both the capability interface here and the registry filter there.
 *
 * Wire format: OpenAI v1 — chat/completions, completions, models. Bearer
 * auth. Streaming via SSE. Providers that diverge from this shape (notably
 * Anthropic) need a sibling backend class — they are not handled here.
 *
 * Selection: recipe="cloud" + the per-model "cloud_provider" field. The
 * Router constructs CloudServer for cloud recipes; ModelManager calls
 * CloudServer::discover_models() at cache-build time, once per provider in
 * config, to populate the available-models list dynamically.
 */
class CloudServer : public WrappedServer {
public:
    CloudServer(const std::string& provider,
                const std::string& log_level,
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

    void forward_streaming_request(const std::string& endpoint,
                                   const std::string& request_body,
                                   httplib::DataSink& sink,
                                   bool sse = true,
                                   long timeout_seconds = 0) override;

    /// Fetch the list of models accessible to this API key from the
    /// provider's /v1/models endpoint. Returns ModelInfos with name,
    /// checkpoint, recipe, cloud_provider, type (inferred from id),
    /// labels, downloaded=true. Empty on any failure (network, auth,
    /// parse) — failures are logged but never thrown so cache build
    /// can continue with other providers.
    static std::vector<ModelInfo> discover_models(const std::string& provider,
                                                   const std::string& api_key,
                                                   const std::string& base_url);

private:
    json post_with_auth(const std::string& path, const json& request, long timeout_seconds = 0);
    json rewrite_model_field(const json& request) const;

    std::string provider_;       // e.g., "fireworks", "openai", "groq"
    std::string base_url_;       // resolved at load time from config
    std::string api_key_;        // resolved at load time
    std::string upstream_model_; // provider's model id (from ModelInfo.checkpoint())
    bool loaded_ = false;
};

} // namespace backends
} // namespace lemon
