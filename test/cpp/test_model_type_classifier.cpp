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
using lemon::infer_labels_from_name;

struct Case {
    const char* name;
    std::vector<std::string> labels;
    ModelType expected;
};

struct InferCase {
    const char* name;
    std::string model_name;
    std::string checkpoint;
    std::vector<std::string> expected;
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

    // --- infer_labels_from_name: substring detection for user-pulled models ---
    const std::vector<InferCase> infer_cases = {
        // "embed" substring triggers embeddings label
        {"embed in name", "zembed-1-Q4_K_M-GGUF", "", {"embeddings"}},
        {"embed in checkpoint", "my-model", "Abiray/zembed-1-Q4_K_M-GGUF:Q4_K_M", {"embeddings"}},
        {"case insensitive", "NOMIC-EMBED-TEXT", "", {"embeddings"}},
        {"checkpoint only (no name)", "", "org/some-embed-model-GGUF:Q4_K_S", {"embeddings"}},

        // "rerank" substring triggers reranking label
        {"rerank in name", "my-reranker-v2", "", {"reranking"}},
        {"rerank in checkpoint", "custom-model", "org/my-reranker-v2:Q8_0", {"reranking"}},

        // Both substrings present
        {"embed and rerank together", "embed-rerank-model", "", {"embeddings", "reranking"}},

        // No match — regular models unaffected
        {"plain LLM", "Qwen3-4B", "Qwen/Qwen3-4B-GGUF:Q4_K_M", {}},
        {"partial overlap not matched", "remember-bot", "", {}},
        {"empty inputs", "", "", {}},
    };

    for (const auto& c : infer_cases) {
        auto actual = infer_labels_from_name(c.model_name, c.checkpoint);
        bool ok = (actual == c.expected);
        if (!ok) {
            std::printf("[FAIL] infer: %s\n", c.name);
            ++failures;
        } else {
            std::printf("[PASS] infer: %s\n", c.name);
        }
    }

    int total = static_cast<int>(cases.size() + infer_cases.size());
    std::printf("\n%d/%d cases passed\n", total - failures, total);
    return failures == 0 ? 0 : 1;
}
