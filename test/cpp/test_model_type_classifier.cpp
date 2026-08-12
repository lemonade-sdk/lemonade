// Standalone test for lemon::get_model_type_from_labels() and
// lemon::ensure_chat_label().
// Compile with: cl /std:c++17 /EHsc /I src/cpp/include test/cpp/test_model_type_classifier.cpp
// or:          g++ -std=c++17 -I src/cpp/include test/cpp/test_model_type_classifier.cpp -o classifier_test

#include "lemon/model_types.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

using lemon::ModelType;
using lemon::ensure_chat_label;
using lemon::get_model_type_from_labels;
using lemon::model_type_to_string;

struct Case {
    const char* name;
    std::vector<std::string> labels;
    ModelType expected;
};

struct StampCase {
    const char* name;
    std::vector<std::string> labels;
    bool expect_chat;
};

int main() {
    const std::vector<Case> cases = {
        // "chat" is the sole chat marker and outranks every other deployment
        // label, so an omni model that also serves ASR still deploys as an LLM.
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

        // chat-transcription means chat with audio input, so it declares a chat
        // model even next to the "transcription" label that selects ASR alone.
        {"chat-transcription alone → LLM", {"chat-transcription"}, ModelType::LLM},
        {"chat-transcription + vision → LLM", {"chat-transcription", "vision"}, ModelType::LLM},
        {"chat-transcription + transcription → LLM",
         {"chat-transcription", "transcription"}, ModelType::LLM},

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
        // FLM), as it looks after ensure_chat_label() stamps it at ingest.
        {"Gemma-4-style any-to-text",
         {"chat", "vision", "reasoning", "tool-calling", "transcription"},
         ModelType::LLM},

        // Fallbacks.
        {"empty labels → LLM", {}, ModelType::LLM},
        {"unknown label → LLM", {"some-future-label"}, ModelType::LLM},
    };

    // ensure_chat_label() stamps the label on models from sources that cannot
    // declare it. Inputs below are real `flm list --json` label sets.
    const std::vector<StampCase> stamp_cases = {
        {"FLM null labels", {}, true},
        {"FLM reasoning-only (deepseek-r1:8b)", {"reasoning"}, true},
        {"FLM vision-only (gemma3:4b)", {"vision"}, true},
        {"FLM gemma4-it any-to-text",
         {"audio", "vision", "reasoning", "tool-calling", "chat-transcription"},
         true},
        {"FLM whisper-v3:turbo stays ASR",
         {"audio", "realtime-transcription", "transcription"},
         false},
        {"FLM embed-gemma:300m stays embedding", {"embeddings"}, false},

        // Descriptor-supplied deployment labels must survive the stamper.
        {"sd-cpp registration", {"custom", "image"}, false},
        {"kokoro registration", {"custom", "tts"}, false},
        {"onnxruntime registration", {"custom", "classification"}, false},

        // chat-transcription declares chat even alongside bare transcription.
        {"chat-transcription + transcription", {"chat-transcription", "transcription"}, true},

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

    for (const auto& c : stamp_cases) {
        std::vector<std::string> labels = c.labels;
        ensure_chat_label(labels);
        const size_t chat_count =
            static_cast<size_t>(std::count(labels.begin(), labels.end(), "chat"));
        const bool ok = (chat_count == (c.expect_chat ? 1u : 0u));
        std::printf("[%s] stamp: %s  (chat x%zu, want x%d)\n",
                    ok ? "PASS" : "FAIL",
                    c.name,
                    chat_count,
                    c.expect_chat ? 1 : 0);
        if (!ok) ++failures;
    }

    const size_t total = cases.size() + stamp_cases.size();
    std::printf("\n%d/%zu cases passed\n", static_cast<int>(total - failures), total);
    return failures == 0 ? 0 : 1;
}
