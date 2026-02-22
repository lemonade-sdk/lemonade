#pragma once

#include <string>
#include <memory>
#include <functional>
#include <nlohmann/json.hpp>
#include <httplib.h>
#include "router.h"
#include "model_manager.h"
#include "model_types.h"

namespace lemon {

using json = nlohmann::json;

class OllamaApi : public std::enable_shared_from_this<OllamaApi> {
public:
    OllamaApi(Router* router, ModelManager* model_manager);

    // Must be called on a shared_ptr instance (uses shared_from_this internally)
    void register_routes(httplib::Server& server);

private:
    Router* router_;
    ModelManager* model_manager_;

    // Endpoint handlers
    void handle_chat(const httplib::Request& req, httplib::Response& res);
    void handle_generate(const httplib::Request& req, httplib::Response& res);
    void handle_generate_image(const json& request_json, httplib::Response& res, const std::string& model);
    void handle_tags(const httplib::Request& req, httplib::Response& res);
    void handle_show(const httplib::Request& req, httplib::Response& res);
    void handle_delete(const httplib::Request& req, httplib::Response& res);
    void handle_pull(const httplib::Request& req, httplib::Response& res);
    void handle_embed(const httplib::Request& req, httplib::Response& res);
    void handle_embeddings(const httplib::Request& req, httplib::Response& res);
    void handle_ps(const httplib::Request& req, httplib::Response& res);
    void handle_version(const httplib::Request& req, httplib::Response& res);

    // Helpers
    void auto_load_model(const std::string& model);
    std::string normalize_model_name(const std::string& name);
    json build_ollama_model_entry(const std::string& id, const ModelInfo& info);
    json convert_openai_chat_to_ollama(const json& openai_response, const std::string& model);
    json convert_openai_delta_to_ollama(const json& openai_chunk, const std::string& model);
    json convert_ollama_to_openai_chat(const json& ollama_request);
    json convert_ollama_to_openai_completion(const json& ollama_request);
    // Common SSE → NDJSON streaming adapter
    using ChunkConverter = std::function<json(const json& openai_chunk)>;
    using DoneBuilder = std::function<json(int prompt_eval_count, int eval_count)>;
    using StreamFn = std::function<void(const std::string& body, httplib::DataSink& sink)>;
    void stream_sse_to_ndjson(const std::string& openai_body,
                              httplib::DataSink& client_sink,
                              ChunkConverter convert_chunk,
                              DoneBuilder build_done,
                              StreamFn call_router);
};

} // namespace lemon
