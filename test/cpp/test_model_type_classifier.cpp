// Standalone test for lemon::get_model_type_from_labels() and
// lemon::find_deployment_mode().
// Compile with: cl /std:c++17 /EHsc /I src/cpp/include test/cpp/test_model_type_classifier.cpp
// or:          g++ -std=c++17 -I src/cpp/include test/cpp/test_model_type_classifier.cpp -o classifier_test

#include "lemon/model_types.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

using lemon::ModelType;
using lemon::find_deployment_mode;
using lemon::get_model_type_from_labels;
using lemon::model_type_to_string;

struct Case {
    const char* name;
    std::vector<std::string> labels;
    ModelType expected;
};

struct DeclaredCase {
    const char* name;
    std::vector<std::string> labels;
    bool expect_declared;
};

int main() {
    const std::vector<Case> cases = {
        // "chat" is the sole chat marker and outranks every other deployment
        // label, so an omni model that also serves ASR still deploys as an LLM.
        // "chat-transcription" is a capability of such a model, not a marker.
        {"chat alone", {"chat"}, ModelType::LLM},
        {"chat + transcription", {"chat", "transcription"}, ModelType::LLM},
        {"chat + embeddings", {"chat", "embeddings"}, ModelType::LLM},
        {"chat + image", {"chat", "image"}, ModelType::LLM},
        {"chat + tts", {"chat", "tts"}, ModelType::LLM},

        // Pure ASR model (e.g. whisper-v3:turbo on FLM). "transcription" label
        // triggers TRANSCRIPTION deployment mode.
        {"whisper-v3:turbo equivalent", {"transcription"}, ModelType::TRANSCRIPTION},
        {"transcription alone", {"transcription"}, ModelType::TRANSCRIPTION},
        {"transcription + realtime", {"transcription", "realtime-transcription"}, ModelType::TRANSCRIPTION},

        // chat-transcription is an input modality, not a mode: it reaches the
        // fallback on its own and does not override a mode that is declared.
        {"chat-transcription alone → LLM", {"chat-transcription"}, ModelType::LLM},
        {"chat-transcription + vision → LLM", {"chat-transcription", "vision"}, ModelType::LLM},
        {"chat + chat-transcription → LLM", {"chat", "chat-transcription"}, ModelType::LLM},
        {"chat-transcription + transcription → TRANSCRIPTION",
         {"chat-transcription", "transcription"}, ModelType::TRANSCRIPTION},

        // Embedding / reranking / image / tts models keep their existing mapping.
        {"embedding (plural)", {"embeddings"}, ModelType::EMBEDDING},
        {"embedding (singular)", {"embedding"}, ModelType::EMBEDDING},
        {"reranking", {"reranking"}, ModelType::RERANKING},
        {"image", {"image"}, ModelType::IMAGE},
        {"tts", {"tts"}, ModelType::TTS},

        // Characteristic labels name no deployment mode, so they reach the
        // fallback rather than being read as chat indicators.
        {"vision-only chat", {"vision"}, ModelType::LLM},
        {"reasoning-only chat", {"reasoning"}, ModelType::LLM},
        {"tool-calling-only chat", {"tool-calling"}, ModelType::LLM},
        {"reasoning + tool-calling", {"reasoning", "tool-calling"}, ModelType::LLM},

        // Multimodal any-to-text chat with transcription label (e.g. Gemma 4 on
        // FLM), as it looks once the FLM discovery path has classified it.
        {"Gemma-4-style any-to-text",
         {"chat", "vision", "reasoning", "tool-calling", "transcription"},
         ModelType::LLM},

        // Fallbacks.
        {"empty labels → LLM", {}, ModelType::LLM},
        {"unknown label → LLM", {"some-future-label"}, ModelType::LLM},
    };

    // find_deployment_mode() is what tells ensure_deployment_label() whether a
    // model already declares a mode or needs its recipe's default stamped on.
    // The FLM inputs below are real `flm list --json` label sets.
    const std::vector<DeclaredCase> declared_cases = {
        {"FLM null labels", {}, false},
        {"FLM reasoning-only (deepseek-r1:8b)", {"reasoning"}, false},
        {"FLM vision-only (gemma3:4b)", {"vision"}, false},
        {"FLM gemma4-it any-to-text",
         {"audio", "vision", "reasoning", "tool-calling", "chat-transcription"},
         false},
        {"FLM whisper-v3:turbo", {"audio", "realtime-transcription", "transcription"}, true},
        {"FLM embed-gemma:300m", {"embeddings"}, true},

        // Descriptor default labels declare the mode at registration.
        {"sd-cpp registration", {"custom", "image"}, true},
        {"kokoro registration", {"custom", "tts"}, true},
        {"onnxruntime registration", {"custom", "classification"}, true},

        {"custom only", {"custom"}, false},
        {"already labeled", {"chat"}, true},
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

    for (const auto& c : declared_cases) {
        ModelType mode = ModelType::LLM;
        const bool declared = find_deployment_mode(c.labels, mode);
        const bool ok = (declared == c.expect_declared);
        std::printf("[%s] declares mode: %s  (got=%s, want=%s)\n",
                    ok ? "PASS" : "FAIL",
                    c.name,
                    declared ? "yes" : "no",
                    c.expect_declared ? "yes" : "no");
        if (!ok) ++failures;
    }

    const size_t total = cases.size() + declared_cases.size();
    std::printf("\n%d/%zu cases passed\n", static_cast<int>(total - failures), total);
    return failures == 0 ? 0 : 1;
}
