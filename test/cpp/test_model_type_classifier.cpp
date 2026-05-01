// Standalone test for lemon::get_model_type_from_labels().
// Compile with: cl /std:c++17 /EHsc /I src/cpp/include test/cpp/test_model_type_classifier.cpp
// or:          g++ -std=c++17 -I src/cpp/include test/cpp/test_model_type_classifier.cpp -o classifier_test

#include "lemon/model_types.h"
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

using lemon::ModelType;
using lemon::get_model_type_from_labels;
using lemon::model_type_to_string;

struct Case {
    const char* name;
    std::vector<std::string> labels;
    ModelType expected;
};

int main() {
    const std::vector<Case> cases = {
        // Pure ASR model (e.g. whisper-v3:turbo on FLM). Audio but no chat
        // indicators → AUDIO deployment mode.
        {"whisper-v3:turbo equivalent", {"audio", "transcription"}, ModelType::AUDIO},
        {"audio alone", {"audio"}, ModelType::AUDIO},

        // Embedding / reranking / image / tts models keep their existing mapping.
        {"embedding (plural)", {"embeddings"}, ModelType::EMBEDDING},
        {"embedding (singular)", {"embedding"}, ModelType::EMBEDDING},
        {"reranking", {"reranking"}, ModelType::RERANKING},
        {"image", {"image"}, ModelType::IMAGE},
        {"tts", {"tts"}, ModelType::TTS},

        // Vision-language chat models (e.g. qwen3vl-it:4b, gemma3:4b).
        {"vision-only chat", {"vision"}, ModelType::LLM},
        {"reasoning-only chat", {"reasoning"}, ModelType::LLM},
        {"tool-calling-only chat", {"tool-calling"}, ModelType::LLM},
        {"reasoning + tool-calling", {"reasoning", "tool-calling"}, ModelType::LLM},

        // The regression we just fixed: multimodal any-to-text chat with audio
        // label (e.g. Gemma 4 on FLM). Must be LLM, not AUDIO.
        {"Gemma-4-style any-to-text",
         {"vision", "reasoning", "tool-calling", "audio", "transcription"},
         ModelType::LLM},
        {"audio + vision only", {"audio", "vision"}, ModelType::LLM},
        {"audio + tool-calling only", {"audio", "tool-calling"}, ModelType::LLM},

        // Fallbacks.
        {"empty labels → LLM", {}, ModelType::LLM},
        {"unknown label → LLM", {"some-future-label"}, ModelType::LLM},
    };

    int failures = 0;
    for (const auto& c : cases) {
        ModelType actual = get_model_type_from_labels(c.labels);
        bool ok = (actual == c.expected);
        std::printf("[%s] %s  (got=%s, want=%s)\n",
                    ok ? "PASS" : "FAIL",
                    c.name,
                    model_type_to_string(actual).c_str(),
                    model_type_to_string(c.expected).c_str());
        if (!ok) ++failures;
    }

    std::printf("\n%d/%zu cases passed\n", static_cast<int>(cases.size() - failures), cases.size());
    return failures == 0 ? 0 : 1;
}
