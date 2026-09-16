#pragma once

#include "lemon/backends/backend_registry.h"

#include "lemon/wrapped_server.h"
#include "lemon/backends/backend_utils.h"
#include <set>
#include <string>

namespace lemon {
namespace backends {

class LlamaCppServer : public WrappedServer, public IEmbeddingsServer, public IRerankingServer, public ISlotsServer, public ITokenizerServer {
public:
    static InstallParams get_install_params(const std::string& backend, const std::string& version);


    LlamaCppServer(const std::string& log_level,
                   ModelManager* model_manager,
                   BackendManager* backend_manager);

    ~LlamaCppServer() override;

    void load(const std::string& model_name,
             const ModelInfo& model_info,
             const RecipeOptions& options,
             bool do_not_upgrade = false) override;

    void unload() override;

    // Downsize the model on soft idle
    bool downsize() override;

    // ICompletionServer implementation
    json chat_completion(const json& request) override;
    json completion(const json& request) override;
    json responses(const json& request) override;

    void forward_streaming_request(const std::string& endpoint,
                                   const std::string& request_body,
                                   httplib::DataSink& sink,
                                   bool sse = true,
                                   long timeout_seconds = 0,
                                   TelemetryCallback telemetry_callback = nullptr) override;

    // IEmbeddingsServer implementation
    json embeddings(const json& request) override;

    // IRerankingServer implementation
    json reranking(const json& request) override;

    // ISlotsServer implementation
    json get_slots() override;
    json slots_action(int slot_id, const std::string& action, const json& request_body) override;

    // ITokenizerServer implementation
    json tokenize(const json& request) override;

protected:
    // Everything a containerized llama-server launch varies on. The image runs
    // the same binary this class drives from disk, so only the launch differs;
    // the forks that ship this way (rocmfpx, llamacpp's nathanw) each fill this
    // in and inherit the rest of the class unchanged.
    struct ContainerLaunch {
        std::string recipe;
        std::string variant;
        std::string profile_id;
        std::string args_option;                      // descriptor option holding custom args
        const std::set<std::string>* reserved_flags;  // flags the caller owns
        int batch_size = 2048;
        int ubatch_size = 2048;
        bool flash_attention = true;
        bool no_mmap = false;
    };

    // Start llama-server inside a pinned OCI image rather than from a local
    // binary, then wait for it the same way load() does.
    void load_containerized(const std::string& model_name, const ModelInfo& model_info,
                            const RecipeOptions& options, const ContainerLaunch& launch);

    // Stop the container started by load_containerized(), if any.
    void unload_containerized(const std::string& recipe, const std::string& variant);

private:
    // The (recipe, variant) whose container is currently loaded, so unload()
    // stops the container by name rather than only signalling the client.
    std::string container_recipe_;
    std::string container_variant_;

    // llama-server echoes the local .gguf path it was launched with (`-m <path>`)
    // in the OpenAI `model` field. Rewrite it to the client-facing model id so
    // responses don't leak absolute filesystem paths (and usernames).
    json normalize_response_model(json response, const json& request) const;
};

namespace llamacpp {
// Factory for the llamacpp backend (constructs the server class — lemond only).
std::unique_ptr<WrappedServer> create(const BackendContext& ctx);
const BackendSpec* spec();
const BackendOps* ops();
constexpr uint32_t capabilities() { return capability_mask_of<LlamaCppServer>(); }
}  // namespace llamacpp
}  // namespace backends
}  // namespace lemon
