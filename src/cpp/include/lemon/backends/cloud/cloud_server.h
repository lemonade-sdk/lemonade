#pragma once

#include "lemon/backends/backend_registry.h"

#include "lemon/model_manager.h"
#include "lemon/utils/http_client.h"
#include "lemon/wrapped_server.h"
#include <string>
#include <vector>

namespace lemon {

class CloudProviderRegistry;

namespace backends {

/**
 * CloudServer offloads inference to a remote OpenAI-compatible provider
 * (Fireworks, OpenAI, Together, Groq, OpenRouter, DeepInfra, etc.) instead
 * of running a local subprocess.
 *
 * Credentials live SERVER-SIDE in CloudProviderRegistry. Resolution priority:
 *   1. LEMONADE_<PROVIDER_UPPER>_API_KEY env var (operator-provisioned)
 *   2. In-memory runtime key set via POST /v1/cloud/auth (ephemeral, dies on
 *      restart; never persisted to disk)
 *   3. None — CloudServer returns a clean 401-shape error
 *
 * Base URL lives in the registry's persisted per-provider record (config.json
 * field "cloud_providers"). Setting LEMONADE_<PROVIDER_UPPER>_BASE_URL has no
 * effect — the URL is registered once via /v1/install.
 *
 * Provider selection: recipe="cloud" + the per-model "cloud_provider" field.
 * The Router constructs CloudServer for cloud recipes, with the provider name
 * fixed by the model's registration. Discovery is server-driven now: at cache
 * build time, ModelManager calls discover_models for every installed provider
 * with a resolvable key, and again whenever a new key is supplied or a
 * provider is installed.
 *
 * Scope: chat-only (chat/completions and completions on OpenAI v1). Other
 * modalities — embeddings, audio, reranking, image — are intentionally not
 * served. discover_models() filters its result to chat-capable ids so the
 * router never sees a cloud model it cannot dispatch.
 *
 * Wire format: OpenAI v1 — chat/completions, completions, models. Bearer
 * auth. Streaming via SSE. Providers that diverge from this shape (notably
 * Anthropic) need a sibling backend class — they are not handled here.
 */
class CloudServer : public WrappedServer {
public:
    CloudServer(const std::string& provider,
                const std::string& log_level,
                ModelManager* model_manager,
                BackendManager* backend_manager,
                CloudProviderRegistry* registry);

    ~CloudServer() override;

    void load(const std::string& model_name,
              const ModelInfo& model_info,
              const RecipeOptions& options,
              bool do_not_upgrade = false) override;

    void unload() override;

    bool is_backend_alive() const override;
    std::string get_backend_health_state() const override;

    json chat_completion(const json& request) override;
    json completion(const json& request) override;
    json responses(const json& request) override;

    void forward_streaming_request(const std::string& endpoint,
                                   const std::string& request_body,
                                   httplib::DataSink& sink,
                                   bool sse = true,
                                   long timeout_seconds = 0,
                                   TelemetryCallback telemetry_callback = nullptr) override;

    /// Fetch the list of models accessible to this API key from the
    /// provider's /v1/models endpoint. Returns ModelInfos with name,
    /// checkpoint, recipe, cloud_provider, type (inferred from id),
    /// labels, downloaded=true. Empty on any failure (network, auth,
    /// parse) — failures are logged but never thrown so cache build
    /// can continue with other providers.
    static std::vector<ModelInfo> discover_models(const std::string& provider,
                                                   const std::string& api_key,
                                                   const std::string& base_url,
                                                   bool allow_insecure_http = false);

    /// Trust boundary for a discovery request. The AllowInsecureHttp opt-in
    /// only applies to plaintext http:// providers; an https:// provider stays
    /// HTTPS-only even if allow_insecure_http is stale or accidentally set, so
    /// a redirect can never downgrade the Bearer-carrying request to http.
    static utils::HttpSecurityPolicy discovery_policy(const std::string& base_url,
                                                      bool allow_insecure_http);

private:
    struct ResolvedCreds {
        std::string api_key;
        std::string base_url;
        bool insecure_http_blocked = false;
    };

    // Looks up creds from the registry. Returns empty fields when the
    // provider is unauthenticated or the registry is missing — callers
    // surface that as a clean 401-shape error.
    ResolvedCreds resolve_creds() const;

    json post_with_auth(const std::string& path, const json& request,
                        const ResolvedCreds& creds, long timeout_seconds = 0);
    json rewrite_model_field(const json& request) const;
    json missing_creds_error() const;
    std::string missing_creds_sse() const;
    json insecure_http_error() const;
    std::string insecure_http_sse() const;

    std::string provider_;       // e.g., "fireworks", "openai", "groq"
    std::string upstream_model_; // provider's model id (from ModelInfo.checkpoint())
    CloudProviderRegistry* registry_ = nullptr;  // Not owned
    bool loaded_ = false;
};

namespace cloud {
// Factory for the cloud backend (constructs the server class — lemond only).
std::unique_ptr<WrappedServer> create(const BackendContext& ctx);
const BackendSpec* spec();
const BackendOps* ops();
}  // namespace cloud
}  // namespace backends
}  // namespace lemon
