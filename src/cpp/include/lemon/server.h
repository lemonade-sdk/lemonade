#pragma once

// CRITICAL: Define thread pool count BEFORE including httplib.h
#ifndef CPPHTTPLIB_THREAD_POOL_COUNT
#define CPPHTTPLIB_THREAD_POOL_COUNT 8
#endif

#include <string>
#include <thread>
#include <memory>
#include <atomic>
#include <chrono>
#include <httplib.h>
#include "router.h"
#include "model_manager.h"

namespace lemon {

class Server {
public:
    Server(int port = 8000,
           const std::string& host = "127.0.0.1",
           const std::string& log_level = "info",
           const json& default_options = json::object(),
           bool tray = false,
           int max_llm_models = 1,
           int max_embedding_models = 1,
           int max_reranking_models = 1,
           int max_audio_models = 1,
           const std::string& extra_models_dir = "");
    
    ~Server();
    
    // Start the server
    void run();
    
    // Stop the server
    void stop();
    
    // Get server status
    bool is_running() const;
    
private:
    std::string resolve_host_to_ip(int ai_family, const std::string& host);
    void setup_routes(httplib::Server &web_server);
    void setup_static_files(httplib::Server &web_server);
    void setup_cors(httplib::Server &web_server);
    void setup_http_logger(httplib::Server &web_server);
    void log_request(const httplib::Request& req);
    httplib::Server::HandlerResponse authenticate_request(const httplib::Request& req, httplib::Response& res);

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
    
    // Helper function for local model resolution and registration
    void resolve_and_register_local_model(
        const std::string& dest_path,
        const std::string& model_name,
        const std::string& recipe,
        const std::string& variant,
        const std::string& mmproj,
        bool reasoning,
        bool& vision,
        bool embedding,
        bool reranking,
        const std::string& hf_cache);

    // Audio endpoint handlers (OpenAI /v1/audio/* compatible)
    void handle_audio_transcriptions(const httplib::Request& req, httplib::Response& res);
    
    // Helper function for auto-loading models (eliminates code duplication and race conditions)
    void auto_load_model_if_needed(const std::string& model_name);
    
    // Helper function to convert ModelInfo to JSON (used by models endpoints)
    nlohmann::json model_info_to_json(const std::string& model_id, const ModelInfo& info);
    
    // Helper function to generate detailed model error responses (not found, not supported, load failure)
    nlohmann::json create_model_error(const std::string& requested_model, const std::string& exception_msg);
    
    int port_;
    std::string host_;
    std::string log_level_;
    json default_options_;
    bool tray_;
    std::string log_file_path_;

    std::thread http_v4_thread_;
    std::thread http_v6_thread_;

    
    std::unique_ptr<httplib::Server> http_server_;
    std::unique_ptr<httplib::Server> http_server_v6_;
    
    std::unique_ptr<Router> router_;
    std::unique_ptr<ModelManager> model_manager_;
    
    bool running_;

    std::string api_key_;
};

} // namespace lemon

