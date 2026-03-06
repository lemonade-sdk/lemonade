#pragma once

#include <string>
#include <vector>
#include <optional>
#include <nlohmann/json.hpp>
#include "model_manager.h"
#include "model_types.h"

namespace lemon {

using json = nlohmann::json;

// Information about a resolved tool call target
struct ExperienceToolInfo {
    std::string tool_name;      // e.g. "generate_image", "text_to_speech"
    std::string target_model;   // The component model name to route to
    ModelType target_type;      // ModelType of the target (IMAGE, TTS, etc.)
};

// Check if model_name is an experience model
bool is_experience_model(const std::string& model_name, ModelManager* mm);

// Find the primary LLM component (first model without image/tts/audio/transcription labels)
// Mirrors frontend's getExperiencePrimaryChatModel()
std::string get_experience_llm_model(const std::string& experience_name, ModelManager* mm);

// Build OpenAI tools array from experience's composite_models
// Image component -> generate_image tool
// TTS component -> text_to_speech tool
json build_experience_tools(const std::string& experience_name, ModelManager* mm);

// Build system prompt instructing LLM about available tools
std::string build_experience_system_prompt(const json& tools);

// Resolve tool call name -> target model name and ModelType
std::optional<ExperienceToolInfo> resolve_tool_call(
    const std::string& function_name,
    const std::string& experience_name,
    ModelManager* mm);

} // namespace lemon
