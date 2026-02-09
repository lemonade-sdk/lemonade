#pragma once

#include "../wrapped_server.h"
#include "backend_utils.h"
#include <string>

namespace lemon {
namespace backends {

class LlamaCppServer : public WrappedServer, public IEmbeddingsServer, public IRerankingServer {
public:
    inline static const BackendSpec SPEC = BackendSpec(
            "llamacpp",
        // executable
    #ifdef _WIN32
            "llama-server.exe"
    #else
            "llama-server"
    #endif
    );

    LlamaCppServer(const std::string& log_level = "info",
                   ModelManager* model_manager = nullptr);

    ~LlamaCppServer() override;

    void install(const std::string& backend = "") override;

    std::string download_model(const std::string& checkpoint,
                              const std::string& mmproj = "",
                              bool do_not_upgrade = false) override;

    void load(const std::string& model_name,
             const ModelInfo& model_info,
             const RecipeOptions& options,
             bool do_not_upgrade = false) override;

    void unload() override;

    // ICompletionServer implementation
    json chat_completion(const json& request) override;
    json completion(const json& request) override;
    json responses(const json& request) override;

    // IEmbeddingsServer implementation
    json embeddings(const json& request) override;

    // IRerankingServer implementation
    json reranking(const json& request) override;
};

} // namespace backends
} // namespace lemon
