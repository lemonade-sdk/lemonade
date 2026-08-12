#pragma once

#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
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
// A model can also be marked as failing, so callers can exercise on_error.
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

    // Make a service throw, so the owning classifier fails (Score{ok=false})
    // and its on_error applies. embed fails per (model, text) since its answers
    // are keyed that way; run_classifier / chat fail per model.
    void set_embed_failure(const std::string& model, const std::string& text) {
        failing_embeds_.insert({model, text});
    }

    void set_classifier_failure(const std::string& model) { failing_classifiers_.insert(model); }

    void set_chat_failure(const std::string& model) { failing_chats_.insert(model); }

    // A call with no configured stub is recorded here instead of being served a
    // placeholder default. A caller that must declare every backend answer (the
    // conformance corpus) checks this is empty and fails the test otherwise, so a
    // missing stub cannot pass on a default value.
    const std::vector<std::string>& unexpected_calls() const { return unexpected_calls_; }

    // Build a ClassifierServices wired to this fake. The returned struct copies
    // `this` by pointer, so keep the FakeClassifierServices alive for the
    // services' lifetime.
    ClassifierServices make() {
        ClassifierServices svc;
        FakeClassifierServices* self = this;
        svc.embed = [self](const std::string& model, const std::string& text) {
            ++self->total_embed_calls_;
            ++self->embed_calls_[text];
            if (self->failing_embeds_.count({model, text}) != 0) {
                throw std::runtime_error("fake embed failure: " + model + " / " + text);
            }
            auto model_it = self->text_embeddings_.find(model);
            if (model_it != self->text_embeddings_.end()) {
                auto text_it = model_it->second.find(text);
                if (text_it != model_it->second.end()) return text_it->second;
            }
            auto it = self->embeddings_.find(model);
            if (it != self->embeddings_.end()) return it->second;
            self->record_unexpected("embed", model + " / " + text);
            return std::vector<float>{1.0f, 0.0f, 0.0f};
        };
        svc.run_classifier = [self](const std::string& model, const std::string&) {
            if (self->failing_classifiers_.count(model) != 0) {
                throw std::runtime_error("fake classifier failure: " + model);
            }
            auto it = self->classifier_scores_.find(model);
            if (it != self->classifier_scores_.end()) return it->second;
            self->record_unexpected("run_classifier", model);
            return std::map<std::string, double>{};
        };
        svc.chat = [self](const std::string& model, const std::string&,
                          const std::string&) {
            if (self->failing_chats_.count(model) != 0) {
                throw std::runtime_error("fake chat failure: " + model);
            }
            auto it = self->chat_replies_.find(model);
            if (it != self->chat_replies_.end()) return it->second;
            self->record_unexpected("chat", model);
            return std::string{};
        };
        return svc;
    }

private:
    void record_unexpected(const std::string& service, const std::string& target) {
        unexpected_calls_.push_back(service + "(" + target + ")");
    }

    std::vector<std::string> unexpected_calls_;
    std::map<std::string, std::vector<float>> embeddings_;
    std::map<std::string, std::map<std::string, std::vector<float>>> text_embeddings_;
    std::map<std::string, int> embed_calls_;
    int total_embed_calls_ = 0;
    std::map<std::string, std::map<std::string, double>> classifier_scores_;
    std::map<std::string, std::string> chat_replies_;
    std::set<std::pair<std::string, std::string>> failing_embeds_;
    std::set<std::string> failing_classifiers_;
    std::set<std::string> failing_chats_;
};

} // namespace testing
} // namespace lemon
