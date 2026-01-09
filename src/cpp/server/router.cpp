#include "lemon/router.h"
#include "lemon/backends/llamacpp_server.h"
#include "lemon/backends/fastflowlm_server.h"
#include "lemon/backends/ryzenaiserver.h"
#include "lemon/backends/whisper_server.h"
#include "lemon/server_capabilities.h"
#include "lemon/error_types.h"
#include <iostream>
#include <algorithm>
#include <filesystem>

namespace lemon {

Router::Router(int ctx_size, const std::string& llamacpp_backend, const std::string& log_level,
               const std::string& llamacpp_args, ModelManager* model_manager,
               int max_llm_models, int max_embedding_models, int max_reranking_models,
               int max_audio_models)
    : ctx_size_(ctx_size), llamacpp_backend_(llamacpp_backend), log_level_(log_level),
      llamacpp_args_(llamacpp_args), model_manager_(model_manager),
      max_llm_models_(max_llm_models), max_embedding_models_(max_embedding_models),
      max_reranking_models_(max_reranking_models), max_audio_models_(max_audio_models) {

    std::cout << "[Router] Multi-model limits: LLM=" << max_llm_models_
              << ", Embedding=" << max_embedding_models_
              << ", Reranking=" << max_reranking_models_
              << ", Audio=" << max_audio_models_ << std::endl;
}

Router::~Router() {
    std::cout << "[Router] Destructor: unloading all models" << std::endl;
    unload_model("");  // Unload all
}

// Helper: Find server by model name
WrappedServer* Router::find_server_by_model_name(const std::string& model_name) const {
    for (const auto& server : loaded_servers_) {
        if (server->get_model_name() == model_name) {
            return server.get();
        }
    }
    return nullptr;
}

// Helper: Get most recently accessed server
WrappedServer* Router::get_most_recent_server() const {
    if (loaded_servers_.empty()) {
        return nullptr;
    }
    
    WrappedServer* most_recent = loaded_servers_[0].get();
    for (const auto& server : loaded_servers_) {
        if (server->get_last_access_time() > most_recent->get_last_access_time()) {
            most_recent = server.get();
        }
    }
    return most_recent;
}

// Helper: Count servers of a given type
int Router::count_servers_by_type(ModelType type) const {
    int count = 0;
    for (const auto& server : loaded_servers_) {
        if (server->get_model_type() == type) {
            count++;
        }
    }
    return count;
}

// Helper: Find LRU server of a given type
WrappedServer* Router::find_lru_server_by_type(ModelType type) const {
    WrappedServer* lru = nullptr;
    
    for (const auto& server : loaded_servers_) {
        if (server->get_model_type() == type) {
            if (!lru || server->get_last_access_time() < lru->get_last_access_time()) {
                lru = server.get();
            }
        }
    }
    
    return lru;
}

// Helper: Check if any server is using NPU
bool Router::has_npu_server() const {
    for (const auto& server : loaded_servers_) {
        if (server->get_device_type() & DEVICE_NPU) {
            return true;
        }
    }
    return false;
}

// Helper: Find any server using NPU
WrappedServer* Router::find_npu_server() const {
    for (const auto& server : loaded_servers_) {
        if (server->get_device_type() & DEVICE_NPU) {
            return server.get();
        }
    }
    return nullptr;
}

// Helper: Evict a specific server
void Router::evict_server(WrappedServer* server) {
    if (!server) return;
    
    std::string model_name = server->get_model_name();
    std::cout << "[Router] Evicting model: " << model_name << std::endl;
    
    // Wait for any ongoing inference to complete
    server->wait_until_not_busy();
    
    // Unload the server
    server->unload();
    
    // Remove from vector
    loaded_servers_.erase(
        std::remove_if(loaded_servers_.begin(), loaded_servers_.end(),
                      [server](const std::unique_ptr<WrappedServer>& s) {
                          return s.get() == server;
                      }),
        loaded_servers_.end()
    );
    
    std::cout << "[Router] Evicted model: " << model_name << std::endl;
}

// Helper: Evict all servers
void Router::evict_all_servers() {
    std::cout << "[Router] Evicting all models (" << loaded_servers_.size() << " total)" << std::endl;
    
    // Wait for all servers to finish
    for (const auto& server : loaded_servers_) {
        server->wait_until_not_busy();
    }
    
    // Unload all
    for (const auto& server : loaded_servers_) {
        std::cout << "[Router] Unloading: " << server->get_model_name() << std::endl;
        server->unload();
    }
    
    loaded_servers_.clear();
    std::cout << "[Router] All models evicted" << std::endl;
}

// Helper: Create backend server based on recipe
std::unique_ptr<WrappedServer> Router::create_backend_server(const ModelInfo& model_info) {
    std::unique_ptr<WrappedServer> new_server;

    if (model_info.recipe == "whispercpp") {
        std::cout << "[Router] Creating WhisperServer backend" << std::endl;
        new_server = std::make_unique<backends::WhisperServer>(log_level_, model_manager_);
    } else if (model_info.recipe == "flm") {
        std::cout << "[Router] Creating FastFlowLM backend" << std::endl;
        new_server = std::make_unique<backends::FastFlowLMServer>(log_level_, model_manager_);
    } else if (model_info.recipe == "oga-npu" || model_info.recipe == "oga-hybrid" ||
               model_info.recipe == "oga-cpu" || model_info.recipe == "ryzenai") {
        std::cout << "[Router] Creating RyzenAI-Server backend: " << model_info.recipe << std::endl;
        
        std::string model_path = model_info.resolved_path;
        std::cout << "[Router] Using model path: " << model_path << std::endl;
        
        std::string backend_mode = model_info.recipe;
        if (model_info.recipe == "oga-npu") {
            backend_mode = "npu";
        } else if (model_info.recipe == "oga-hybrid") {
            backend_mode = "hybrid";
        } else if (model_info.recipe == "oga-cpu") {
            backend_mode = "cpu";
        } else {
            backend_mode = "auto";
        }
        
        auto* ryzenai_server = new RyzenAIServer(model_info.model_name, 
                                                  log_level_ == "debug", model_manager_);
        ryzenai_server->set_model_path(model_path);
        ryzenai_server->set_execution_mode(backend_mode);
        new_server.reset(ryzenai_server);
    } else {
        std::cout << "[Router] Creating LlamaCpp backend" << std::endl;
        new_server = std::make_unique<backends::LlamaCppServer>(log_level_, model_manager_);
    }
    
    return new_server;
}

std::string Router::prioritized_option(const std::string& load_option, const std::string& model_option, const std::string& fallback) {
    if (!load_option.empty()) {
        return load_option;
    }

    if (!model_option.empty()) {
        return model_option;
    }

    return fallback;
}

int Router::prioritized_option(int load_option, int model_option, int fallback) {
    if (load_option >= 0) {
        return load_option;
    }

    if (model_option >= 0) {
        return model_option;
    }

    return fallback;
}

void Router::load_model(const std::string& model_name,
                       const ModelInfo& model_info,
                       bool do_not_upgrade,
                       int ctx_size_override,
                       const std::string& llamacpp_backend_override,
                       const std::string& llamacpp_args_override) {
    
    // Resolve settings: load overrides take precedence over per-model overrides which take precedence over defaults
    int effective_ctx_size = prioritized_option(ctx_size_override, model_info.ctx_size, ctx_size_);
    std::string effective_backend = prioritized_option(llamacpp_backend_override, model_info.llamacpp_backend, llamacpp_backend_);
    std::string effective_args = prioritized_option(llamacpp_args_override, model_info.llamacpp_args, llamacpp_args_);
    
    std::cout << "[Router] Effective settings: ctx_size=" << effective_ctx_size 
              << ", backend=" << effective_backend 
              << ", args=" << (effective_args.empty() ? "(none)" : effective_args) << std::endl;
    
    // LOAD SERIALIZATION STRATEGY (from spec: point #2 in Additional Considerations)
    std::unique_lock<std::mutex> lock(load_mutex_);
    
    // Wait if another thread is currently loading
    while (is_loading_) {
        std::cout << "[Router] Another load is in progress, waiting..." << std::endl;
        load_cv_.wait(lock);
    }
    
    // Mark that we're now loading (prevents concurrent loads)
    is_loading_ = true;
    
    std::cout << "[Router] Loading model: " << model_name 
              << " (checkpoint: " << model_info.checkpoint 
              << ", recipe: " << model_info.recipe 
              << ", type: " << model_type_to_string(model_info.type)
              << ", device: " << device_type_to_string(model_info.device) << ")" << std::endl;
    
    try {
        // Check if model is already loaded
        WrappedServer* existing = find_server_by_model_name(model_name);
        if (existing) {
            std::cout << "[Router] Model already loaded, updating access time" << std::endl;
            existing->update_access_time();
            is_loading_ = false;
            load_cv_.notify_all();
            return;
        }
        
        // Determine model type and device
        ModelType model_type = model_info.type;
        DeviceType device_type = model_info.device;
        
        // Get max models for this type
        int max_models = 0;
        switch (model_type) {
            case ModelType::LLM:
                max_models = max_llm_models_;
                break;
            case ModelType::EMBEDDING:
                max_models = max_embedding_models_;
                break;
            case ModelType::RERANKING:
                max_models = max_reranking_models_;
                break;
            case ModelType::AUDIO:
                max_models = max_audio_models_;
                break;
        }
        
        // NPU EXCLUSIVITY CHECK (from spec: Additional NPU Rules)
        if (device_type & DEVICE_NPU) {
            WrappedServer* npu_server = find_npu_server();
            if (npu_server) {
                std::cout << "[Router] NPU is occupied by: " << npu_server->get_model_name() 
                          << ", evicting..." << std::endl;
                evict_server(npu_server);
            }
        }
        
        // LRU EVICTION CHECK (from spec: Least Recently Used Cache)
        int current_count = count_servers_by_type(model_type);
        if (current_count >= max_models) {
            WrappedServer* lru = find_lru_server_by_type(model_type);
            if (lru) {
                std::cout << "[Router] Slot limit reached for type " 
                          << model_type_to_string(model_type)
                          << ", evicting LRU: " << lru->get_model_name() << std::endl;
                evict_server(lru);
            }
        }
        
        // Create new backend server
        std::unique_ptr<WrappedServer> new_server = create_backend_server(model_info);
        
        // Set model metadata
        new_server->set_model_metadata(model_name, model_info.checkpoint, model_type, device_type);
        new_server->update_access_time();
        
        // CRITICAL: Release lock before slow backend startup
        lock.unlock();
        
        // Load the backend (this can take 30-60 seconds)
        std::cout << "[Router] Starting backend (this may take a moment)..." << std::endl;
        bool load_success = false;
        std::string error_message;
        
        try {
            new_server->load(model_name, model_info, effective_ctx_size, do_not_upgrade, 
                           effective_backend, effective_args);
            load_success = true;
            std::cout << "[Router] Backend started successfully" << std::endl;
        } catch (const std::exception& e) {
            error_message = e.what();
            load_success = false;
            std::cout << "[Router] Backend load failed: " << error_message << std::endl;
        }
        
        // Re-acquire lock for final state update
        lock.lock();
        
        if (load_success) {
            // Success: Add to loaded servers
            loaded_servers_.push_back(std::move(new_server));
            
            is_loading_ = false;
            load_cv_.notify_all();
            
            std::cout << "[Router] Model loaded successfully. Total loaded: " 
                      << loaded_servers_.size() << std::endl;
        } else {
            // ERROR HANDLING (from spec: Error Handling section)
            // Check if error is "file not found" (exception to nuclear policy)
            bool is_file_not_found = (error_message.find("not found") != std::string::npos ||
                                     error_message.find("does not exist") != std::string::npos ||
                                     error_message.find("No such file") != std::string::npos);
            
            // Check if error is "model invalidated" (e.g., FLM upgrade invalidated model files)
            // This should NOT trigger retry - user must manually re-download the model
            bool is_model_invalidated = (error_message.find("was invalidated") != std::string::npos);
            
            is_loading_ = false;
            load_cv_.notify_all();
            
            if (is_file_not_found) {
                std::cout << "[Router] File not found error, NOT evicting other models" << std::endl;
                throw std::runtime_error(error_message);
            }
            
            if (is_model_invalidated) {
                std::cout << "[Router] Model invalidated error, NOT retrying (user must re-download)" << std::endl;
                throw std::runtime_error(error_message);
            }
            
            // Nuclear option: evict all models and retry
            std::cout << "[Router] Load failed with non-file-not-found error, "
                      << "evicting all models and retrying..." << std::endl;
            
            evict_all_servers();
            
            // Mark loading again for retry
            is_loading_ = true;
            
            // Create new server for retry
            std::unique_ptr<WrappedServer> retry_server = create_backend_server(model_info);
            retry_server->set_model_metadata(model_name, model_info.checkpoint, model_type, device_type);
            retry_server->update_access_time();
            
            // Release lock for retry
            lock.unlock();
            
            std::cout << "[Router] Retrying backend load..." << std::endl;
            try {
                retry_server->load(model_name, model_info, effective_ctx_size, do_not_upgrade,
                                 effective_backend, effective_args);
                
                // Re-acquire lock
                lock.lock();
                
                loaded_servers_.push_back(std::move(retry_server));
                is_loading_ = false;
                load_cv_.notify_all();
                
                std::cout << "[Router] Retry successful!" << std::endl;
            } catch (const std::exception& retry_error) {
                lock.lock();
                is_loading_ = false;
                load_cv_.notify_all();
                
                std::cerr << "[Router] Retry also failed: " << retry_error.what() << std::endl;
                throw;
            }
        }
        
    } catch (const std::exception& e) {
        std::cerr << "[Router ERROR] Failed to load model: " << e.what() << std::endl;
        
        if (!lock.owns_lock()) {
            lock.lock();
        }
        is_loading_ = false;
        load_cv_.notify_all();
        
        throw;
    }
}

void Router::unload_model(const std::string& model_name) {
    std::lock_guard<std::mutex> lock(load_mutex_);
    
    if (model_name.empty()) {
        // Unload all models
        std::cout << "[Router] Unload all models called" << std::endl;
        evict_all_servers();
    } else {
        // Unload specific model
        std::cout << "[Router] Unload model called: " << model_name << std::endl;
        WrappedServer* server = find_server_by_model_name(model_name);
        if (!server) {
            throw std::runtime_error("Model not loaded: " + model_name);
        }
        evict_server(server);
    }
}

std::string Router::get_loaded_model() const {
    std::lock_guard<std::mutex> lock(load_mutex_);
    WrappedServer* server = get_most_recent_server();
    return server ? server->get_model_name() : "";
}

std::string Router::get_loaded_checkpoint() const {
    std::lock_guard<std::mutex> lock(load_mutex_);
    WrappedServer* server = get_most_recent_server();
    return server ? server->get_checkpoint() : "";
}

std::string Router::get_loaded_recipe() const {
    std::lock_guard<std::mutex> lock(load_mutex_);
    WrappedServer* server = get_most_recent_server();
    if (!server) return "";
    
    // Infer recipe from device type (reverse mapping)
    DeviceType device = server->get_device_type();
    if (device == DEVICE_GPU) return "llamacpp";
    if (device == (DEVICE_GPU | DEVICE_NPU)) return "oga-hybrid";
    if (device == DEVICE_NPU) return "flm";  // Could be flm or oga-npu
    if (device == DEVICE_CPU) return "oga-cpu";
    
    return "";
}

json Router::get_all_loaded_models() const {
    std::lock_guard<std::mutex> lock(load_mutex_);
    
    json result = json::array();
    
    for (const auto& server : loaded_servers_) {
        json model_info;
        model_info["model_name"] = server->get_model_name();
        model_info["checkpoint"] = server->get_checkpoint();
        model_info["type"] = model_type_to_string(server->get_model_type());
        model_info["device"] = device_type_to_string(server->get_device_type());
        model_info["backend_url"] = server->get_address();  // For debugging port issues
        
        // Convert timestamp to milliseconds since epoch
        auto time_point = server->get_last_access_time();
        auto duration = time_point.time_since_epoch();
        auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
        model_info["last_use"] = millis;
        
        result.push_back(model_info);
    }
    
    return result;
}

json Router::get_max_model_limits() const {
    return {
        {"llm", max_llm_models_},
        {"embedding", max_embedding_models_},
        {"reranking", max_reranking_models_}
    };
}

bool Router::is_model_loaded() const {
    std::lock_guard<std::mutex> lock(load_mutex_);
    return !loaded_servers_.empty();
}

bool Router::is_model_loaded(const std::string& model_name) const {
    std::lock_guard<std::mutex> lock(load_mutex_);
    return find_server_by_model_name(model_name) != nullptr;
}

ModelType Router::get_model_type(const std::string& model_name) const {
    std::lock_guard<std::mutex> lock(load_mutex_);
    WrappedServer* server = model_name.empty()
        ? get_most_recent_server()
        : find_server_by_model_name(model_name);
    return server ? server->get_model_type() : ModelType::LLM;
}

std::string Router::get_backend_address() const {
    std::lock_guard<std::mutex> lock(load_mutex_);
    WrappedServer* server = get_most_recent_server();
    return server ? server->get_address() : "";
}

// Template method for generic inference execution
template<typename Func>
auto Router::execute_inference(const json& request, Func&& inference_func) -> decltype(inference_func(nullptr)) {
    WrappedServer* server = nullptr;
    
    {
        std::lock_guard<std::mutex> lock(load_mutex_);
        
        // Extract model from request - required field, no fallback to avoid silent misrouting
        std::string requested_model;
        if (request.contains("model") && request["model"].is_string()) {
            requested_model = request["model"].get<std::string>();
        }
        
        if (requested_model.empty()) {
            return ErrorResponse::from_exception(InvalidRequestException("No model specified in request"));
        }
        
        server = find_server_by_model_name(requested_model);
        if (!server) {
            return ErrorResponse::from_exception(ModelNotLoadedException(requested_model));
        }
        
        // Mark as busy and update access time
        server->set_busy(true);
        server->update_access_time();
    } // Lock released here
    
    // Execute inference without holding lock (but busy flag prevents eviction)
    try {
        auto response = inference_func(server);
        server->set_busy(false);
        return response;
    } catch (...) {
        server->set_busy(false);
        throw;
    }
}

// Template method for streaming execution
template<typename Func>
void Router::execute_streaming(const std::string& request_body, httplib::DataSink& sink, Func&& streaming_func) {
    WrappedServer* server = nullptr;
    
    {
        std::lock_guard<std::mutex> lock(load_mutex_);
        
        // Extract model from request body if present (same logic as execute_inference)
        std::string requested_model;
        try {
            json request = json::parse(request_body);
            if (request.contains("model") && request["model"].is_string()) {
                requested_model = request["model"].get<std::string>();
            }
        } catch (...) {
            // If JSON parsing fails, fall back to most recent server
            std::cerr << "[Router DEBUG] Failed to parse request body for model extraction" << std::endl;
        }
        
        // Find requested model - no fallback to avoid silent misrouting
        if (requested_model.empty()) {
            std::cerr << "[Router ERROR] No model specified in streaming request" << std::endl;
            std::string error_msg = "data: {\"error\":{\"message\":\"No model specified in request\",\"type\":\"invalid_request_error\"}}\n\n";
            sink.write(error_msg.c_str(), error_msg.size());
            return;
        }
        
        server = find_server_by_model_name(requested_model);
        if (!server) {
            std::string error_msg = "data: {\"error\":{\"message\":\"Model not loaded: " + requested_model + "\",\"type\":\"model_not_loaded\"}}\n\n";
            sink.write(error_msg.c_str(), error_msg.size());
            return;
        }
        
        server->set_busy(true);
        server->update_access_time();
    }
    
    try {
        streaming_func(server);
        server->set_busy(false);
    } catch (...) {
        server->set_busy(false);
        throw;
    }
}

json Router::chat_completion(const json& request) {
    return execute_inference(request, [&](WrappedServer* server) {
        return server->chat_completion(request);
    });
}

json Router::completion(const json& request) {
    return execute_inference(request, [&](WrappedServer* server) {
        return server->completion(request);
    });
}

json Router::embeddings(const json& request) {
    return execute_inference(request, [&](WrappedServer* server) {
        auto embeddings_server = dynamic_cast<IEmbeddingsServer*>(server);
        if (!embeddings_server) {
            return ErrorResponse::from_exception(
                UnsupportedOperationException("Embeddings", device_type_to_string(server->get_device_type()))
            );
        }
        return embeddings_server->embeddings(request);
    });
}

json Router::reranking(const json& request) {
    return execute_inference(request, [&](WrappedServer* server) {
        auto reranking_server = dynamic_cast<IRerankingServer*>(server);
        if (!reranking_server) {
            return ErrorResponse::from_exception(
                UnsupportedOperationException("Reranking", device_type_to_string(server->get_device_type()))
            );
        }
        return reranking_server->reranking(request);
    });
}

json Router::responses(const json& request) {
    return execute_inference(request, [&](WrappedServer* server) {
        return server->responses(request);
    });
}

json Router::audio_transcriptions(const json& request) {
    return execute_inference(request, [&](WrappedServer* server) {
        auto audio_server = dynamic_cast<IAudioServer*>(server);
        if (!audio_server) {
            return ErrorResponse::from_exception(
                UnsupportedOperationException("Audio transcription", device_type_to_string(server->get_device_type()))
            );
        }
        return audio_server->audio_transcriptions(request);
    });
}

json Router::get_stats() const {
    std::lock_guard<std::mutex> lock(load_mutex_);
    WrappedServer* server = get_most_recent_server();
    if (!server) {
        return ErrorResponse::from_exception(ModelNotLoadedException());
    }
    return server->get_telemetry().to_json();
}

void Router::update_telemetry(int input_tokens, int output_tokens, 
                              double time_to_first_token, double tokens_per_second) {
    std::lock_guard<std::mutex> lock(load_mutex_);
    WrappedServer* server = get_most_recent_server();
    if (server) {
        server->set_telemetry(input_tokens, output_tokens, 
                             time_to_first_token, tokens_per_second);
    }
}

void Router::update_prompt_tokens(int prompt_tokens) {
    std::lock_guard<std::mutex> lock(load_mutex_);
    WrappedServer* server = get_most_recent_server();
    if (server) {
        server->set_prompt_tokens(prompt_tokens);
    }
}

void Router::chat_completion_stream(const std::string& request_body, httplib::DataSink& sink) {
    execute_streaming(request_body, sink, [&](WrappedServer* server) {
        server->forward_streaming_request("/v1/chat/completions", request_body, sink);
    });
}

void Router::completion_stream(const std::string& request_body, httplib::DataSink& sink) {
    execute_streaming(request_body, sink, [&](WrappedServer* server) {
        server->forward_streaming_request("/v1/completions", request_body, sink);
    });
}

void Router::responses_stream(const std::string& request_body, httplib::DataSink& sink) {
    execute_streaming(request_body, sink, [&](WrappedServer* server) {
        server->forward_streaming_request("/v1/responses", request_body, sink);
    });
}

} // namespace lemon
