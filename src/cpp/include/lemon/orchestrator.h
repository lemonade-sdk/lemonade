#pragma once

#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "router.h"
#include "model_manager.h"

namespace lemon {

using json = nlohmann::json;

// The Orchestrator exposes Lemonade's endpoints (transcription, image gen,
// TTS, embeddings, reranking) as tools for a local LLM.  It auto-selects
// the right model for each tool based on platform presets, so the user
// only needs to describe what they want in natural language.
//
// Flow:
//   1. User sends a prompt to POST /orchestrate
//   2. Orchestrator loads a platform-appropriate LLM as the "brain"
//   3. The LLM receives tool definitions for each available endpoint
//   4. If the LLM emits tool_calls, the Orchestrator executes them
//      against the local Router and feeds results back
//   5. The loop repeats until the LLM produces a final text response
class Orchestrator {
public:
    Orchestrator(Router* router, ModelManager* model_manager);

    // Resolve which platform preset matches the current hardware.
    // Called once at server startup after system info is available.
    void resolve_platform_preset();

    // Run the orchestration loop for a user request.
    // The request JSON follows the /chat/completions schema with
    // optional extra fields:
    //   "orchestrator_model"  – override the preset's orchestrator LLM
    //   "preset"              – force a specific preset by name
    //   "max_iterations"      – cap on tool-calling rounds (default 10)
    json orchestrate(const json& request);

    // Return the resolved preset info for diagnostics / GET /orchestrate/info
    json get_preset_info() const;

    // Return the tool definitions that describe available endpoints
    json get_endpoint_tools() const;

private:
    // Build the static tool definitions once (called from constructor)
    void build_endpoint_tools();

    // Load presets from platform_presets.json resource file
    json load_presets_file() const;

    // Ensure a model is loaded (download if needed, then load via router)
    void ensure_model_loaded(const std::string& model_name);

    // Execute a single tool_call and return the result as a string
    std::string execute_tool_call(const json& tool_call);

    // Individual tool executors
    std::string execute_transcribe_audio(const json& arguments, const json& context);
    std::string execute_generate_image(const json& arguments);
    std::string execute_text_to_speech(const json& arguments);
    std::string execute_compute_embeddings(const json& arguments);
    std::string execute_rerank_documents(const json& arguments);

    Router* router_;
    ModelManager* model_manager_;

    json preset_;                    // Resolved platform preset
    json tools_;                     // Tool definitions array
    std::string orchestrator_model_; // Model name for the orchestrating LLM
    json endpoint_models_;           // Model name per endpoint capability
    int max_iterations_ = 10;

    // Context carried across the orchestration loop (e.g., uploaded audio)
    json orchestration_context_;
};

} // namespace lemon
