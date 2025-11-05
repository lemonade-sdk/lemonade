#include "lemon/router.h"
#include "lemon/backends/llamacpp_server.h"
#include "lemon/backends/fastflowlm_server.h"
#include "lemon/backends/ryzenaiserver.h"
#include "lemon/error_types.h"
#include <iostream>
#include <algorithm>
#include <filesystem>

namespace lemon {

Router::Router(int ctx_size, const std::string& llamacpp_backend, const std::string& log_level)
    : ctx_size_(ctx_size), llamacpp_backend_(llamacpp_backend), log_level_(log_level) {
}

Router::~Router() {
    // Only unload if it hasn't been explicitly unloaded already
    // (Server::stop() calls unload_model() explicitly for graceful shutdown)
    if (wrapped_server_ && !unload_called_) {
        std::cout << "[Router] Destructor: unloading model" << std::endl;
        unload_model();
    }
}

void Router::load_model(const std::string& model_name,
                       const ModelInfo& model_info,
                       bool do_not_upgrade) {
    
    // LOAD SERIALIZATION STRATEGY
    // ============================
    // POLICY: Only ONE load operation can execute at a time.
    //
    // Why: Concurrent loads create orphaned backend processes when both try to swap
    // into wrapped_server_. The last one wins, but the first's process keeps running.
    //
    // Implementation:
    //   1. Use is_loading_ flag to track if a load is in progress
    //   2. Other threads wait on load_cv_ condition variable until load completes
    //   3. Release lock during slow operations (backend startup) to allow:
    //      - Health checks to read wrapped_server_
    //      - Inference requests to use current model
    //      - Stats/system-info queries
    //   4. Waiting threads remain blocked on load_cv_ even when lock is released
    //
    // Result: Load operations are serialized (no orphaned processes), but the server
    // remains responsive during loading.
    
    std::unique_lock<std::mutex> lock(load_mutex_);
    
    // Wait if another thread is currently loading
    // The condition variable will automatically release the lock while waiting,
    // and re-acquire it when notified
    while (is_loading_) {
        std::cout << "[Router] Another load is in progress, waiting..." << std::endl;
        load_cv_.wait(lock);
    }
    
    // Mark that we're now loading (prevents concurrent loads)
    is_loading_ = true;
    
    std::cout << "[Router] Loading model: " << model_name << " (checkpoint: " << model_info.checkpoint << ", recipe: " << model_info.recipe << ")" << std::endl;
    
    try {
        // Unload any existing model (quick operation, keep lock)
        if (wrapped_server_) {
            std::cout << "[Router] Unloading previous model..." << std::endl;
            unload_model();
        }
        
        // Create the backend server object (quick operation)
        std::unique_ptr<WrappedServer> new_server;
        
        // Determine which backend to use based on recipe
        if (model_info.recipe == "flm") {
            std::cout << "[Router] Using FastFlowLM backend" << std::endl;
            new_server = std::make_unique<backends::FastFlowLMServer>(log_level_);
        } else if (model_info.recipe == "oga-npu" || model_info.recipe == "oga-hybrid" || model_info.recipe == "oga-cpu" || model_info.recipe == "ryzenai") {
            std::cout << "[Router] Using RyzenAI-Server backend: " << model_info.recipe << std::endl;
            
            // RyzenAI-Server needs the model path - use pre-resolved path
            std::string model_path = model_info.resolved_path;
            std::cout << "[Router] Using model path: " << model_path << std::endl;
            
            // Determine backend mode based on recipe
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
            
            auto* ryzenai_server = new RyzenAIServer(model_name, 8080, log_level_ == "debug");
            ryzenai_server->set_model_path(model_path);
            ryzenai_server->set_execution_mode(backend_mode);
            new_server.reset(ryzenai_server);
        } else {
            std::cout << "[Router] Using LlamaCpp backend: " << llamacpp_backend_ << std::endl;
            new_server = std::make_unique<backends::LlamaCppServer>(llamacpp_backend_, log_level_);
        }
        
        // CRITICAL: Release the lock before the time-consuming backend startup
        // ======================================================================
        // Why we release the lock here:
        //   - Backend startup (process creation + health checks) can take 30-60 seconds
        //   - If we hold the lock, other operations would block:
        //     * Health checks couldn't read wrapped_server_
        //     * Inference requests couldn't use the current model
        //     * Stats/system-info queries would hang
        //
        // Safety with is_loading_ flag:
        //   - is_loading_=true prevents other threads from starting concurrent loads
        //   - Threads waiting on load_cv_ will remain blocked even after lock is released
        //   - Only after we set is_loading_=false and notify will waiting threads proceed
        //   - This guarantees NO orphaned processes (loads are fully serialized)
        lock.unlock();
        
        // Load the backend (start process, wait for ready) WITHOUT holding the mutex
        // This is the time-consuming part that can take 30-60 seconds:
        //   1. ProcessManager::start_process() - Launch llama-server/ryzenai-server/flm
        //   2. wait_for_ready() - Poll health endpoint until backend responds
        std::cout << "[Router] Starting backend (this may take a moment)..." << std::endl;
        new_server->load(model_name, model_info, ctx_size_, do_not_upgrade);
        std::cout << "[Router] Backend started successfully" << std::endl;
        
        // Re-acquire the lock for the final state update
        lock.lock();
        
        // Swap in the new server as the active backend
        wrapped_server_ = std::move(new_server);
        loaded_model_ = model_name;
        loaded_checkpoint_ = model_info.checkpoint;
        loaded_recipe_ = model_info.recipe;
        unload_called_ = false;  // Reset unload flag for newly loaded model
        
        // CRITICAL: Mark loading as complete and notify waiting threads
        // Without this, threads waiting on load_cv_ would deadlock forever!
        is_loading_ = false;
        load_cv_.notify_all();  // Wake up all threads waiting to load
        
        std::cout << "[Router] Model loaded successfully" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[Router ERROR] Failed to load model: " << e.what() << std::endl;
        
        // CRITICAL: Mark loading as complete even on error!
        // If we don't do this, threads waiting on load_cv_ will deadlock forever.
        // We need to re-acquire the lock if we don't currently hold it
        if (!lock.owns_lock()) {
            lock.lock();
        }
        is_loading_ = false;
        load_cv_.notify_all();
        
        if (wrapped_server_) {
            wrapped_server_.reset();
        }
        throw;  // Re-throw to propagate up
    }
}

void Router::unload_model() {
    std::cout << "[Router] Unload model called" << std::endl;
    if (wrapped_server_ && !unload_called_) {
        std::cout << "[Router] Calling wrapped_server->unload()" << std::endl;
        wrapped_server_->unload();
        wrapped_server_.reset();
        loaded_model_.clear();
        loaded_checkpoint_.clear();
        loaded_recipe_.clear();
        unload_called_ = true;  // Mark as unloaded
        std::cout << "[Router] Wrapped server cleaned up" << std::endl;
    } else if (unload_called_) {
        std::cout << "[Router] Model already unloaded (skipping)" << std::endl;
    } else {
        std::cout << "[Router] No wrapped server to unload" << std::endl;
    }
}

std::string Router::get_backend_address() const {
    if (!wrapped_server_) {
        return "";
    }
    return wrapped_server_->get_address();
}

json Router::chat_completion(const json& request) {
    if (!wrapped_server_) {
        return ErrorResponse::from_exception(ModelNotLoadedException());
    }
    return wrapped_server_->chat_completion(request);
}

json Router::completion(const json& request) {
    if (!wrapped_server_) {
        return ErrorResponse::from_exception(ModelNotLoadedException());
    }
    return wrapped_server_->completion(request);
}

json Router::embeddings(const json& request) {
    if (!wrapped_server_) {
        return ErrorResponse::from_exception(ModelNotLoadedException());
    }
    
    auto embeddings_server = dynamic_cast<IEmbeddingsServer*>(wrapped_server_.get());
    if (!embeddings_server) {
        return ErrorResponse::from_exception(
            UnsupportedOperationException("Embeddings", loaded_recipe_)
        );
    }
    
    return embeddings_server->embeddings(request);
}

json Router::reranking(const json& request) {
    if (!wrapped_server_) {
        return ErrorResponse::from_exception(ModelNotLoadedException());
    }
    
    auto reranking_server = dynamic_cast<IRerankingServer*>(wrapped_server_.get());
    if (!reranking_server) {
        return ErrorResponse::from_exception(
            UnsupportedOperationException("Reranking", loaded_recipe_)
        );
    }
    
    return reranking_server->reranking(request);
}

json Router::responses(const json& request) {
    if (!wrapped_server_) {
        return ErrorResponse::from_exception(ModelNotLoadedException());
    }
    return wrapped_server_->responses(request);
}

json Router::get_stats() const {
    if (!wrapped_server_) {
        return ErrorResponse::from_exception(ModelNotLoadedException());
    }
    return wrapped_server_->get_telemetry().to_json();
}

void Router::chat_completion_stream(const std::string& request_body, httplib::DataSink& sink) {
    if (!wrapped_server_) {
        std::string error_msg = "{\"error\":{\"message\":\"No model loaded\",\"type\":\"model_not_loaded\"}}\n";
        sink.write(error_msg.c_str(), error_msg.size());
        return;
    }
    wrapped_server_->forward_streaming_request("/v1/chat/completions", request_body, sink);
}

void Router::completion_stream(const std::string& request_body, httplib::DataSink& sink) {
    if (!wrapped_server_) {
        std::string error_msg = "{\"error\":{\"message\":\"No model loaded\",\"type\":\"model_not_loaded\"}}\n";
        sink.write(error_msg.c_str(), error_msg.size());
        return;
    }
    wrapped_server_->forward_streaming_request("/v1/completions", request_body, sink);
}

void Router::responses_stream(const std::string& request_body, httplib::DataSink& sink) {
    if (!wrapped_server_) {
        std::string error_msg = "{\"error\":{\"message\":\"No model loaded\",\"type\":\"model_not_loaded\"}}\n";
        sink.write(error_msg.c_str(), error_msg.size());
        return;
    }
    wrapped_server_->forward_streaming_request("/v1/responses", request_body, sink);
}

} // namespace lemon

