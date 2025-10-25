#pragma once

#include "../wrapped_server.h"
#include <string>

namespace lemon {
namespace backends {

class LlamaCppServer : public WrappedServer, public IEmbeddingsServer, public IRerankingServer {
public:
    LlamaCppServer(const std::string& backend = "vulkan", const std::string& log_level = "info");
    
    ~LlamaCppServer() override;
    
    void install(const std::string& backend = "") override;
    
    std::string download_model(const std::string& checkpoint,
                              const std::string& mmproj = "",
                              bool do_not_upgrade = false) override;
    
    void load(const std::string& model_name,
             const std::string& checkpoint,
             const std::string& mmproj,
             int ctx_size,
             bool do_not_upgrade = false,
             const std::vector<std::string>& labels = {}) override;
    
    void unload() override;
    
    // ICompletionServer implementation
    json chat_completion(const json& request) override;
    json completion(const json& request) override;
    json responses(const json& request) override;
    
    // IEmbeddingsServer implementation
    json embeddings(const json& request) override;
    
    // IRerankingServer implementation
    json reranking(const json& request) override;
    
protected:
    void parse_telemetry(const std::string& line) override;
    
private:
    std::string get_llama_server_path();
    std::string find_gguf_file(const std::string& checkpoint);
    
    std::string backend_;  // vulkan, rocm, metal
    std::string model_path_;
};

} // namespace backends
} // namespace lemon

