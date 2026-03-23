#pragma once

#include <string>
#include <vector>
#include <set>
#include <nlohmann/json.hpp>
#include "model_manager.h"

namespace lemon {

using json = nlohmann::json;

// Check if model_name is an experience model
bool is_experience_model(const std::string& model_name, ModelManager* mm);

// Find the primary LLM component (first model without image/tts/audio/transcription labels)
// Mirrors frontend's getExperiencePrimaryChatModel()
std::string get_experience_llm_model(const std::string& experience_name, ModelManager* mm);

// Check if a set of labels contains any label from a target set
bool has_label(const std::vector<std::string>& labels, const std::set<std::string>& target);

} // namespace lemon
