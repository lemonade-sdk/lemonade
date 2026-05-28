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
 * name comes from the per-model "cloud_provider" field; base URL and API key
 * are supplied per-request by the client (or via LEMONADE_<PROVIDER>_BASE_URL
 * and LEMONADE_<PROVIDER>_API_KEY env vars as a server-side fallback).
 *
 * Per-client credentials: lemond does NOT persist cloud keys. Each client
 * (desktop app, CLI, third-party SDK) supplies its own credentials per
 * request via the X-Lemonade-Cloud-Key and X-Lemonade-Cloud-Base-Url
 * headers. The server.cpp chat handlers extract these and inject them into
 * the request body as the "_lemonade_cloud_creds" field; CloudServer reads
 * and strips the field before forwarding upstream. This mirrors how the
 * lemonade client API key works (per-client storage, sent per request) and
 * honors Invariant #11 in AGENTS.md.
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
 * Router constructs CloudServer for cloud recipes. Discovery is now
 * client-driven via POST /internal/cloud/discover — the server no longer
 * auto-populates cloud models at cache build time.
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
    // Per-request credentials extracted from "_lemonade_cloud_creds" field
    // (injected by server.cpp from X-Lemonade-Cloud-* headers) with env-var
    // fallback. The base_url has its trailing slash stripped so path
    // concatenation doesn't produce "//chat/...".
    struct PerRequestCreds {
        std::string api_key;
        std::string base_url;
    };

    // Extracts and strips "_lemonade_cloud_creds" from the request (mutates).
    // Falls back to LEMONADE_<PROVIDER>_API_KEY / _BASE_URL env vars for any
    // missing field. Returns the resolved creds; api_key or base_url may
    // still be empty if neither header nor env var supplied them.
    PerRequestCreds extract_creds(json& request) const;

    json post_with_auth(const std::string& path, const json& request,
                        const PerRequestCreds& creds, long timeout_seconds = 0);
    json rewrite_model_field(const json& request) const;

    std::string provider_;       // e.g., "fireworks", "openai", "groq"
    std::string upstream_model_; // provider's model id (from ModelInfo.checkpoint())
    bool loaded_ = false;
};

} // namespace backends
} // namespace lemon
