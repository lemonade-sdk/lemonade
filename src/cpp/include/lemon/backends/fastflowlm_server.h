#pragma once

#include "../wrapped_server.h"
#include <string>

namespace lemon {
namespace backends {

class FastFlowLMServer : public WrappedServer, public IEmbeddingsServer, public IRerankingServer {
public:
    FastFlowLMServer(const std::string& log_level = "info");
    
    ~FastFlowLMServer() override;
    
    void install(const std::string& backend = "") override;
    
    std::string download_model(const std::string& checkpoint,
                              const std::string& mmproj = "",
                              bool do_not_upgrade = false) override;
    
    void load(const std::string& model_name,
             const ModelInfo& model_info,
             int ctx_size,
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
    
    // FLM uses /api/tags for readiness check instead of /health
    bool wait_for_ready() override;
    
protected:
    void parse_telemetry(const std::string& line) override;
    
private:
    // Existing methods
    std::string get_flm_path();
    bool check_npu_available();
    
    // Version management
    std::string get_flm_latest_version();
    std::pair<std::string, std::string> check_flm_version(); // returns (current, latest)
    bool compare_versions(const std::string& v1, const std::string& v2); // true if v1 >= v2
    
    // Installation
    void install_or_upgrade_flm();
    bool download_flm_installer(const std::string& output_path);
    void run_flm_installer(const std::string& installer_path, bool silent);
    
    // Environment management
    void refresh_environment_path();
    bool verify_flm_installation(const std::string& expected_version, int max_retries = 10);
    
    std::string model_name_;
    bool is_loaded_ = false;
};

} // namespace backends
} // namespace lemon

