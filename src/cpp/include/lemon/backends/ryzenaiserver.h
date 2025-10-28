#pragma once

#include "lemon/wrapped_server.h"
#include "lemon/server_capabilities.h"
#include "lemon/error_types.h"
#include <string>

namespace lemon {

class RyzenAIServer : public WrappedServer {
public:
    RyzenAIServer(const std::string& model_name, int port, bool debug);
    ~RyzenAIServer() override;
    
    // Installation and availability
    static bool is_available();
    static std::string get_ryzenai_serve_path();
    
    // WrappedServer interface
    void install(const std::string& backend = "") override;
    void parse_telemetry(const std::string& line) override;
    
    // Model operations - Note: RyzenAI-Serve loads model at startup
    std::string download_model(const std::string& checkpoint,
                              const std::string& mmproj = "",
                              bool do_not_upgrade = true) override;
    
    void load(const std::string& model_name,
             const std::string& checkpoint,
             const std::string& mmproj,
             int ctx_size,
             bool do_not_upgrade = false,
             const std::vector<std::string>& labels = {}) override;
    
    // RyzenAI-specific: set execution mode before loading
    void set_execution_mode(const std::string& mode) { execution_mode_ = mode; }
    
    // RyzenAI-specific: set model path before loading
    void set_model_path(const std::string& path) { model_path_ = path; }
    
    void unload() override;
    
    // Inference operations (from ICompletionServer via WrappedServer)
    json chat_completion(const json& request) override;
    json completion(const json& request) override;
    json responses(const json& request) override;

private:
    std::string model_name_;
    std::string model_path_;
    std::string execution_mode_; // "auto", "npu", or "hybrid"
    bool is_loaded_;
    
    // Helper to download and install ryzenai-serve
    static void download_and_install();
    
    // Helper to determine best execution mode based on model
    std::string determine_execution_mode(const std::string& model_path,
                                        const std::string& backend);
};

} // namespace lemon

