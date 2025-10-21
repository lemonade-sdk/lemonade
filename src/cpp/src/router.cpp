#include "lemon/router.h"
#include "lemon/backends/llamacpp_server.h"
#include "lemon/backends/fastflowlm_server.h"
#include <iostream>

namespace lemon {

Router::Router(int ctx_size, const std::string& llamacpp_backend)
    : ctx_size_(ctx_size), llamacpp_backend_(llamacpp_backend) {
}

Router::~Router() {
    if (wrapped_server_) {
        unload_model();
    }
}

void Router::load_model(const std::string& model_name,
                       const std::string& checkpoint,
                       const std::string& recipe,
                       bool do_not_upgrade) {
    // TODO: Implement model loading logic
    std::cout << "[Router] Loading model: " << model_name << std::endl;
}

void Router::unload_model() {
    if (wrapped_server_) {
        wrapped_server_->unload();
        wrapped_server_.reset();
        loaded_model_.clear();
        loaded_checkpoint_.clear();
        loaded_recipe_.clear();
    }
}

json Router::chat_completion(const json& request) {
    if (!wrapped_server_) {
        return {{"error", "No model loaded"}};
    }
    return wrapped_server_->chat_completion(request);
}

json Router::completion(const json& request) {
    if (!wrapped_server_) {
        return {{"error", "No model loaded"}};
    }
    return wrapped_server_->completion(request);
}

json Router::embeddings(const json& request) {
    if (!wrapped_server_) {
        return {{"error", "No model loaded"}};
    }
    return wrapped_server_->embeddings(request);
}

json Router::reranking(const json& request) {
    if (!wrapped_server_) {
        return {{"error", "No model loaded"}};
    }
    return wrapped_server_->reranking(request);
}

json Router::get_stats() const {
    if (!wrapped_server_) {
        return {{"error", "No model loaded"}};
    }
    return wrapped_server_->get_telemetry().to_json();
}

} // namespace lemon

