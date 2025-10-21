#pragma once

#include "../wrapped_server.h"
#include <string>

namespace lemon {
namespace backends {

class FastFlowLMServer : public WrappedServer {
public:
    FastFlowLMServer(const std::string& log_level = "info");
    
    ~FastFlowLMServer() override;
    
    void install(const std::string& backend = "") override;
    
    std::string download_model(const std::string& checkpoint,
                              const std::string& mmproj = "",
                              bool do_not_upgrade = false) override;
    
    void load(const std::string& model_name,
             const std::string& checkpoint,
             const std::string& mmproj,
             int ctx_size,
             bool do_not_upgrade = false) override;
    
    void unload() override;
    
    json chat_completion(const json& request) override;
    json completion(const json& request) override;
    json embeddings(const json& request) override;
    json reranking(const json& request) override;
    
    // FLM uses /api/tags for readiness check instead of /health
    bool wait_for_ready() override;
    
protected:
    void parse_telemetry(const std::string& line) override;
    
private:
    std::string get_flm_path();
    bool check_npu_available();
    std::string model_name_;
    bool is_loaded_ = false;
};

} // namespace backends
} // namespace lemon

