#pragma once

#include "../wrapped_server.h"
#include <string>

namespace lemon {
namespace backends {

class LlamaCppServer : public WrappedServer, public IEmbeddingsServer, public IRerankingServer {
public:
    LlamaCppServer(const std::string& log_level = "info",
                   ModelManager* model_manager = nullptr);
    
    ~LlamaCppServer() override;
    
    void install(const std::string& backend = "") override;
    
    std::string download_model(const std::string& checkpoint,
                              const std::string& mmproj = "",
                              bool do_not_upgrade = false) override;
    
    void load(const std::string& model_name,
             const ModelInfo& model_info,
             int ctx_size,
             bool do_not_upgrade = false,
             const std::string& llamacpp_backend = "vulkan",
             const std::string& llamacpp_args = "") override;
    
    void unload() override;
    
    // ICompletionServer implementation
    json chat_completion(const json& request) override;
    json completion(const json& request) override;
    json responses(const json& request) override;
    
    // IEmbeddingsServer implementation
    json embeddings(const json& request) override;
    
    // IRerankingServer implementation
    json reranking(const json& request) override;
    
private:
    std::string get_llama_server_path(const std::string& backend);
    std::string find_executable_in_install_dir(const std::string& install_dir);
    std::string find_external_llama_server(const std::string& backend);
};

} // namespace backends
} // namespace lemon

