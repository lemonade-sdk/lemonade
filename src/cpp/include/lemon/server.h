#pragma once

// CRITICAL: Define thread pool count BEFORE including httplib.h
#ifndef CPPHTTPLIB_THREAD_POOL_COUNT
#define CPPHTTPLIB_THREAD_POOL_COUNT 8
#endif

#include <string>
#include <memory>
#include <httplib.h>
#include "router.h"
#include "model_manager.h"

namespace lemon {

class Server {
public:
    Server(int port = 8000, 
           const std::string& host = "127.0.0.1",
           const std::string& log_level = "info",
           int ctx_size = 4096,
           bool tray = false,
           const std::string& llamacpp_backend = "vulkan",
           const std::string& llamacpp_args = "",
           int max_llm_models = 1,
           int max_embedding_models = 1,
           int max_reranking_models = 1);
    
    ~Server();
    
    // Start the server
    void run();
    
    // Stop the server
    void stop();
    
    // Get server status
    bool is_running() const;
    
private:
    void setup_routes();
    void setup_static_files();
    void setup_cors();
    
    // Endpoint handlers
    void handle_health(const httplib::Request& req, httplib::Response& res);
    void handle_models(const httplib::Request& req, httplib::Response& res);
    void handle_model_by_id(const httplib::Request& req, httplib::Response& res);
    void handle_chat_completions(const httplib::Request& req, httplib::Response& res);
    void handle_completions(const httplib::Request& req, httplib::Response& res);
    void handle_embeddings(const httplib::Request& req, httplib::Response& res);
    void handle_reranking(const httplib::Request& req, httplib::Response& res);
    void handle_responses(const httplib::Request& req, httplib::Response& res);
    void handle_pull(const httplib::Request& req, httplib::Response& res);
    void handle_load(const httplib::Request& req, httplib::Response& res);
    void handle_unload(const httplib::Request& req, httplib::Response& res);
    void handle_delete(const httplib::Request& req, httplib::Response& res);
    void handle_params(const httplib::Request& req, httplib::Response& res);
    void handle_stats(const httplib::Request& req, httplib::Response& res);
    void handle_system_info(const httplib::Request& req, httplib::Response& res);
    void handle_log_level(const httplib::Request& req, httplib::Response& res);
    void handle_shutdown(const httplib::Request& req, httplib::Response& res);
    void handle_logs_stream(const httplib::Request& req, httplib::Response& res);
    void handle_add_local_model(const httplib::Request& req, httplib::Response& res);
    
    // Helper function for auto-loading models (eliminates code duplication and race conditions)
    void auto_load_model_if_needed(const std::string& model_name);
    
    // Helper function to convert ModelInfo to JSON (used by models endpoints)
    nlohmann::json model_info_to_json(const std::string& model_id, const ModelInfo& info);
    
    int port_;
    std::string host_;
    std::string log_level_;
    int ctx_size_;
    bool tray_;
    std::string llamacpp_backend_;
    std::string llamacpp_args_;
    std::string log_file_path_;
    
    std::unique_ptr<httplib::Server> http_server_;
    std::unique_ptr<Router> router_;
    std::unique_ptr<ModelManager> model_manager_;
    
    bool running_;
};

} // namespace lemon

