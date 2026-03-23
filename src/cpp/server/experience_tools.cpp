#include "lemon/experience_tools.h"
#include <lemon/utils/aixlog.hpp>
#include <algorithm>
#include <set>

namespace lemon {

// Labels that indicate non-LLM models (mirrors frontend NON_LLM_LABELS)
static const std::set<std::string> NON_LLM_LABELS = {
    "image", "speech", "tts", "audio", "transcription",
    "embeddings", "embedding", "reranking"
};

bool has_label(const std::vector<std::string>& labels, const std::set<std::string>& target) {
    for (const auto& label : labels) {
        if (target.count(label)) return true;
    }
    return false;
}

bool is_experience_model(const std::string& model_name, ModelManager* mm) {
    if (!mm->model_exists(model_name)) return false;
    auto info = mm->get_model_info(model_name);
    return info.recipe == "experience" && !info.composite_models.empty();
}

std::string get_experience_llm_model(const std::string& experience_name, ModelManager* mm) {
    auto info = mm->get_model_info(experience_name);

    for (const auto& component : info.composite_models) {
        if (!mm->model_exists(component)) continue;
        auto comp_info = mm->get_model_info(component);
        bool is_non_llm = false;
        for (const auto& label : comp_info.labels) {
            if (NON_LLM_LABELS.count(label)) {
                is_non_llm = true;
                break;
            }
        }
        if (!is_non_llm) return component;
    }

    // Fallback to first component
    return info.composite_models.empty() ? experience_name : info.composite_models[0];
}

} // namespace lemon
