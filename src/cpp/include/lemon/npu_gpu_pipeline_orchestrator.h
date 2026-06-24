#pragma once

#include <functional>
#include <string>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "model_manager.h"
#include "router.h"

namespace lemon {

using json = nlohmann::json;

// Server-side Strix Halo style pipeline for collection.npu_gpu models.
//
// A collection.npu_gpu model has exactly two components:
//   1. an FLM/NPU draft model
//   2. a GPU verifier model (typically llamacpp, including MTP-labelled GGUFs)
//
// Request flow:
//   - Generate a short draft/prefix on the NPU.
//   - Append that draft as an assistant prefill for the verifier.
//   - Ask the GPU verifier to continue. If the verifier echoes the prefill,
//     strip the duplicate so clients see the draft only once.
class NpuGpuPipelineOrchestrator {
public:
    using AutoLoadFn = std::function<void(const std::string&)>;

    NpuGpuPipelineOrchestrator(Router& router,
                               ModelManager& model_manager,
                               AutoLoadFn auto_load);

    json chat_completion(const json& request, const ModelInfo& collection_info);
    void chat_completion_stream(const json& request,
                                const ModelInfo& collection_info,
                                httplib::DataSink& sink);

private:
    struct Components {
        std::string draft_model;
        std::string verifier_model;
    };

    Components resolve_components(const ModelInfo& collection_info) const;
    int resolve_max_tokens(const json& request) const;
    int resolve_draft_tokens(const json& request,
                             const ModelInfo& collection_info,
                             int max_tokens) const;
    json make_draft_request(const json& request,
                            const std::string& draft_model,
                            int draft_tokens) const;
    json make_verifier_request(const json& request,
                               const std::string& verifier_model,
                               const std::string& draft_text,
                               int verifier_tokens,
                               bool stream) const;
    std::string extract_message_content(const json& response) const;
    std::string strip_duplicate_prefix(const std::string& text,
                                       const std::string& prefix) const;
    json make_response(const json& request,
                       const ModelInfo& collection_info,
                       const std::string& content,
                       const json& draft_response,
                       const json& verifier_response) const;
    void write_sse_chunk(httplib::DataSink& sink,
                         const std::string& id,
                         long created,
                         const std::string& model,
                         const std::string& content,
                         bool final) const;

    Router& router_;
    ModelManager& model_manager_;
    AutoLoadFn auto_load_;
};

} // namespace lemon
