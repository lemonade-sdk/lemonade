#pragma once

#include <map>
#include <string>
#include <vector>
#include "lemon/routing_policy.h"

// A behavior-free fake ClassifierServices for routing-engine unit tests. Tests
// that exercise the contract surface — the match-expression evaluator, the
// classifier registry, the individual classifiers — bind the engine to this
// instead of the live Router so they run with no backend subprocess.
//
// It returns fixed, caller-configured outputs:
//   - embed(model, text)          -> a fixed vector (default: one configured per
//                                    model, else a deterministic unit vector).
//   - run_classifier(model, text) -> a fixed label->score map per model.
//   - chat(model, prompt, input)  -> a fixed reply per model.
//
// Nothing here implements routing or scoring logic; tests dictate every output.

namespace lemon {
namespace testing {

class FakeClassifierServices {
public:
    // Configure a fixed embedding vector returned for `model`.
    void set_embedding(const std::string& model, std::vector<float> vec) {
        embeddings_[model] = std::move(vec);
    }

    // Configure a fixed embedding for a specific (model, text) pair. Takes
    // precedence over the per-model default, letting tests give each reference
    // phrase and the input their own vector.
    void set_embedding(const std::string& model, const std::string& text,
                       std::vector<float> vec) {
        text_embeddings_[model][text] = std::move(vec);
    }

    // Number of embed() calls observed for `text` (across all models).
    int embed_calls(const std::string& text) const {
        auto it = embed_calls_.find(text);
        return it == embed_calls_.end() ? 0 : it->second;
    }

    // Total embed() calls observed.
    int total_embed_calls() const { return total_embed_calls_; }

    // Configure a fixed label->score map returned for `model`.
    void set_classifier_scores(const std::string& model,
                               std::map<std::string, double> scores) {
        classifier_scores_[model] = std::move(scores);
    }

    // Configure a fixed chat reply returned for `model`.
    void set_chat_reply(const std::string& model, std::string reply) {
        chat_replies_[model] = std::move(reply);
    }

    // Build a ClassifierServices wired to this fake. The returned struct copies
    // `this` by pointer, so keep the FakeClassifierServices alive for the
    // services' lifetime.
    ClassifierServices make() {
        ClassifierServices svc;
        FakeClassifierServices* self = this;
        svc.embed = [self](const std::string& model, const std::string& text) {
            ++self->total_embed_calls_;
            ++self->embed_calls_[text];
            auto model_it = self->text_embeddings_.find(model);
            if (model_it != self->text_embeddings_.end()) {
                auto text_it = model_it->second.find(text);
                if (text_it != model_it->second.end()) return text_it->second;
            }
            auto it = self->embeddings_.find(model);
            if (it != self->embeddings_.end()) return it->second;
            return std::vector<float>{1.0f, 0.0f, 0.0f};
        };
        svc.run_classifier = [self](const std::string& model, const std::string&) {
            auto it = self->classifier_scores_.find(model);
            if (it != self->classifier_scores_.end()) return it->second;
            return std::map<std::string, double>{};
        };
        svc.chat = [self](const std::string& model, const std::string&,
                          const std::string&) {
            auto it = self->chat_replies_.find(model);
            if (it != self->chat_replies_.end()) return it->second;
            return std::string{};
        };
        return svc;
    }

private:
    std::map<std::string, std::vector<float>> embeddings_;
    std::map<std::string, std::map<std::string, std::vector<float>>> text_embeddings_;
    std::map<std::string, int> embed_calls_;
    int total_embed_calls_ = 0;
    std::map<std::string, std::map<std::string, double>> classifier_scores_;
    std::map<std::string, std::string> chat_replies_;
};

} // namespace testing
} // namespace lemon
