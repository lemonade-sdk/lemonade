#pragma once

#include "../wrapped_server.h"
#include "backend_utils.h"

#include <mutex>
#include <string>
#include <vector>
#include <unordered_set>

namespace lemon {
namespace backends {

// LlamaCppRouter launches a single llama-server in router mode and forwards
// all llamacpp HTTP traffic to it. Unlike LlamaCppServer (one process per
// model), LlamaCppRouter hosts a roster of models sourced from either a
// --models-preset .ini file or a --models-dir directory, and therefore:
//
//   - owns_model(name)      -> true iff `name` is in the roster
//   - get_owned_models()    -> full roster
//   - is_evictable()        -> false (pinned for process lifetime)
//
// The instance is installed into Router via install_router_server() at server
// startup. The Router's load guard ensures llamacpp /load requests that miss
// the roster are rejected rather than silently spawning a second llama-server.
class LlamaCppRouter : public WrappedServer,
                       public IEmbeddingsServer,
                       public IRerankingServer {
public:
    // Reuse the same llama-server binary managed by BackendManager/LlamaCppServer.
    LlamaCppRouter(const std::string& log_level,
                   ModelManager* model_manager,
                   BackendManager* backend_manager);

    ~LlamaCppRouter() override;

    // Start the router llama-server. `model_info.recipe` is ignored — router
    // mode is driven entirely by RuntimeConfig (router_models_preset /
    // router_models_dir / router_default_args). The `model_name` and
    // `model_info` parameters are retained for API compatibility with
    // WrappedServer::load.
    void load(const std::string& model_name,
              const ModelInfo& model_info,
              const RecipeOptions& options,
              bool do_not_upgrade = false) override;

    // Convenience: start the router without needing a dummy ModelInfo.
    // Used by Server startup (server.cpp) for the install_router_server() path.
    void start();

    void unload() override;

    // ICompletionServer
    json chat_completion(const json& request) override;
    json completion(const json& request) override;
    json responses(const json& request) override;

    // IEmbeddingsServer
    json embeddings(const json& request) override;

    // IRerankingServer
    json reranking(const json& request) override;

    // --- WrappedServer multi-model overrides ---
    bool owns_model(const std::string& name) const override;
    std::vector<std::string> get_owned_models() const override;
    bool is_evictable() const override { return false; }

    // Refresh the cached roster by querying the llama-server child's
    // /v1/models endpoint. Safe to call at any time; no-op if the child
    // is not running.
    void refresh_roster();

private:
    // Resolve which llamacpp backend variant (cpu / vulkan / rocm-* / metal)
    // the router process should be launched with. Mirrors LlamaCppServer.
    std::string resolve_backend_choice() const;

    // Build the argv for the llama-server child process.
    std::vector<std::string> build_args(const std::string& llamacpp_backend) const;

    // Build the env vars for the llama-server child process (LD_LIBRARY_PATH
    // for ROCm etc.). Mirrors LlamaCppServer.
    std::vector<std::pair<std::string, std::string>> build_env_vars(
        const std::string& llamacpp_backend,
        const std::string& executable) const;

    // Current roster. Guarded by roster_mutex_.
    mutable std::mutex roster_mutex_;
    std::unordered_set<std::string> roster_;
};

} // namespace backends
} // namespace lemon
