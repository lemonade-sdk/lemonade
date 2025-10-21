#include "lemon/backends/fastflowlm_server.h"
#include <iostream>

namespace lemon {
namespace backends {

FastFlowLMServer::FastFlowLMServer(const std::string& log_level)
    : WrappedServer("FastFlowLM", log_level) {
}

FastFlowLMServer::~FastFlowLMServer() {
    unload();
}

void FastFlowLMServer::install(const std::string& backend) {
    // TODO: Implement installation check
    std::cout << "[FastFlowLM] Install check" << std::endl;
}

std::string FastFlowLMServer::download_model(const std::string& checkpoint,
                                            const std::string& mmproj,
                                            bool do_not_upgrade) {
    // TODO: Implement model download with FLM
    return checkpoint;
}

void FastFlowLMServer::load(const std::string& model_name,
                           const std::string& checkpoint,
                           const std::string& mmproj,
                           int ctx_size,
                           bool do_not_upgrade) {
    // TODO: Implement model loading
    std::cout << "[FastFlowLM] Loading model: " << model_name << std::endl;
}

void FastFlowLMServer::unload() {
    // TODO: Implement unload
}

json FastFlowLMServer::chat_completion(const json& request) {
    // TODO: Implement
    return {{"error", "Not implemented"}};
}

json FastFlowLMServer::completion(const json& request) {
    // TODO: Implement
    return {{"error", "Not implemented"}};
}

json FastFlowLMServer::embeddings(const json& request) {
    // TODO: Implement
    return {{"error", "Not implemented"}};
}

json FastFlowLMServer::reranking(const json& request) {
    // TODO: Implement
    return {{"error", "Not implemented"}};
}

void FastFlowLMServer::parse_telemetry(const std::string& line) {
    // TODO: Implement telemetry parsing
}

std::string FastFlowLMServer::get_flm_path() {
    // TODO: Implement path detection
    return "flm";
}

bool FastFlowLMServer::check_npu_available() {
    // TODO: Implement NPU detection
    return false;
}

} // namespace backends
} // namespace lemon

