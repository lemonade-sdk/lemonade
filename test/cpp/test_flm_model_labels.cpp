#include "lemon/backends/fastflowlm/flm_model_labels.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

using lemon::backends::fastflowlm::add_flm_mode_labels;

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (condition) {
        std::cout << "ok: " << message << std::endl;
        return;
    }
    std::cerr << "FAIL: " << message << std::endl;
    ++failures;
}

bool has(const std::vector<std::string>& labels, const std::string& label) {
    return std::find(labels.begin(), labels.end(), label) != labels.end();
}

std::vector<std::string> labelled(const std::string& checkpoint,
                                  std::vector<std::string> labels) {
    add_flm_mode_labels(checkpoint, labels);
    return labels;
}

}  // namespace

int main() {
    const auto d3 = labelled("d3:lite", {"decision"});
    expect(has(d3, "classification") && has(d3, "systemone"),
           "a decision model deploys as a SystemOne classifier");
    expect(!has(d3, "chat"), "a decision model is not a chat model");
    expect(has(d3, "decision"), "FLM's own labels are kept");

    expect(labelled("whisper-v3:turbo", {}) == std::vector<std::string>{"transcription"},
           "whisper is a transcription model");
    expect(labelled("embed-gemma:300m", {}) == std::vector<std::string>{"embeddings"},
           "embed-gemma is an embedding model");
    expect(labelled("EMBED-Gemma:300m", {}) == std::vector<std::string>{"embeddings"},
           "the checkpoint name is matched without case");

    const auto chat = labelled("qwen3.5:4b", {"reasoning", "vision"});
    expect(has(chat, "chat") && !has(chat, "classification"),
           "any other model is a chat model");
    expect(labelled("llama3.2:1b", {"chat"}) == std::vector<std::string>{"chat"},
           "a chat label is not added twice");

    if (failures != 0) {
        std::cerr << failures << " check(s) failed" << std::endl;
        return 1;
    }
    std::cout << "all checks passed" << std::endl;
    return 0;
}
