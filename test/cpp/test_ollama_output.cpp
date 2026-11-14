#include "lemon/server/gateway_conversion.h"

#include <cstdio>
#include <string>
#include <vector>

using lemon::OllamaOutput;
using lemon::ollama_output;

struct TestResult {
    int passed = 0;
    int failed = 0;

    void check(bool condition, const std::string& name) {
        printf("[%s] %s\n", condition ? "PASS" : "FAIL", name.c_str());
        if (condition) {
            ++passed;
        } else {
            ++failed;
        }
    }
};

struct Case {
    const char* name;
    std::vector<std::string> labels;
    OllamaOutput expected;
};

static void test_ollama_output(TestResult& r) {
    const Case cases[] = {
        {"chat", {"chat"}, OllamaOutput::Text},
        {"chat + vision + tool-calling", {"chat", "vision", "tool-calling"}, OllamaOutput::Text},
        {"chat + image", {"chat", "image"}, OllamaOutput::Text},
        {"chat-transcription + vision", {"chat-transcription", "vision"}, OllamaOutput::Text},
        {"no labels", {}, OllamaOutput::Text},

        {"image", {"image"}, OllamaOutput::Image},
        {"image + edit", {"image", "edit"}, OllamaOutput::Image},
        {"custom + image", {"custom", "image"}, OllamaOutput::Image},

        {"upscaling + on-demand + image", {"upscaling", "on-demand", "image"}, OllamaOutput::Nothing},
        {"image + upscaling", {"image", "upscaling"}, OllamaOutput::Nothing},
        {"embeddings", {"embeddings"}, OllamaOutput::Nothing},
        {"reranking", {"reranking"}, OllamaOutput::Nothing},
        {"transcription", {"transcription", "realtime-transcription"}, OllamaOutput::Nothing},
        {"tts", {"tts"}, OllamaOutput::Nothing},
        {"classification + systemone", {"classification", "systemone"}, OllamaOutput::Nothing},
        {"audio-generation", {"audio-generation"}, OllamaOutput::Nothing},
        {"3d", {"3d"}, OllamaOutput::Nothing},
    };

    for (const auto& c : cases) {
        r.check(ollama_output(c.labels) == c.expected, c.name);
    }
}

int main() {
    TestResult r;
    printf("=== Ollama Output Classification Unit Tests ===\n\n");

    test_ollama_output(r);

    printf("\n%d/%d tests passed\n", r.passed, r.passed + r.failed);
    return r.failed == 0 ? 0 : 1;
}
