#include "lemon/router.h"
#include "lemon/backends/llamacpp_server.h"
#include "lemon/backends/fastflowlm_server.h"
#include "lemon/error_types.h"
#include <iostream>

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
                       const std::string& checkpoint,
                       const std::string& recipe,
                       bool do_not_upgrade) {
    
    std::cout << "[Router] Loading model: " << model_name << " (checkpoint: " << checkpoint << ", recipe: " << recipe << ")" << std::endl;
    
    try {
        // Unload any existing model
        if (wrapped_server_) {
            std::cout << "[Router] Unloading previous model..." << std::endl;
            unload_model();
        }
        
        // Determine which backend to use based on recipe
        if (recipe == "flm") {
            std::cout << "[Router] Using FastFlowLM backend" << std::endl;
            wrapped_server_ = std::make_unique<backends::FastFlowLMServer>(log_level_);
        } else {
            std::cout << "[Router] Using LlamaCpp backend: " << llamacpp_backend_ << std::endl;
            wrapped_server_ = std::make_unique<backends::LlamaCppServer>(llamacpp_backend_, log_level_);
        }
        
        // Load the model
        wrapped_server_->load(model_name, checkpoint, "", ctx_size_, do_not_upgrade);
        
        loaded_model_ = model_name;
        loaded_checkpoint_ = checkpoint;
        loaded_recipe_ = recipe;
        unload_called_ = false;  // Reset unload flag for newly loaded model
        
        std::cout << "[Router] Model loaded successfully" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[Router ERROR] Failed to load model: " << e.what() << std::endl;
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

json Router::get_stats() const {
    if (!wrapped_server_) {
        return ErrorResponse::from_exception(ModelNotLoadedException());
    }
    return wrapped_server_->get_telemetry().to_json();
}

} // namespace lemon

