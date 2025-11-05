#pragma once

#include "types.h"
#include "inference_engine.h"
#include <memory>
#include <httplib.h>

namespace ryzenai {

class RyzenAIServer {
public:
    explicit RyzenAIServer(const CommandLineArgs& args);
    ~RyzenAIServer();
    
    // Start the server (blocks until stopped)
    void run();
    
    // Stop the server
    void stop();
    
private:
    void loadModel();
    void setupRoutes();
    
    // Helper to create GenerationParams with hierarchy: user provided > search config > defaults
    GenerationParams createGenerationParams(int max_tokens, float temperature, float top_p, 
                                           int top_k, float repeat_penalty, 
                                           const std::vector<std::string>& stop) const;
    std::string extractModelName(const std::string& model_path);
    
    // Route handlers
    void handleHealth(const httplib::Request& req, httplib::Response& res);
    void handleCompletions(const httplib::Request& req, httplib::Response& res);
    void handleChatCompletions(const httplib::Request& req, httplib::Response& res);
    void handleResponses(const httplib::Request& req, httplib::Response& res);
    
    // Helper methods
    json createErrorResponse(const std::string& message, const std::string& type);
    void setupCORS(httplib::Response& res);
    
    std::unique_ptr<httplib::Server> http_server_;
    std::unique_ptr<InferenceEngine> inference_engine_;
    
    CommandLineArgs args_;
    std::string model_id_;
    bool running_ = false;
};

} // namespace ryzenai

