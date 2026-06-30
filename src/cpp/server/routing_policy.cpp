#include "lemon/routing_policy.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>
#include <lemon/utils/aixlog.hpp>

namespace lemon {
namespace {

Score failed_score() {
    Score score;
    score.ok = false;
    return score;
}

// Cosine similarity of two equal-length, non-zero vectors. Returns nullopt when
// the vectors are empty, mismatched in length, or either has zero magnitude —
// all of which the caller treats as a classifier failure (Score::ok=false).
std::optional<double> cosine_similarity(const std::vector<float>& a,
                                        const std::vector<float>& b) {
    if (a.empty() || a.size() != b.size()) {
        return std::nullopt;
    }
    double dot = 0.0;
    double norm_a = 0.0;
    double norm_b = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const double ai = static_cast<double>(a[i]);
        const double bi = static_cast<double>(b[i]);
        dot += ai * bi;
        norm_a += ai * ai;
        norm_b += bi * bi;
    }
    if (norm_a <= 0.0 || norm_b <= 0.0) {
        return std::nullopt;
    }
    return dot / (std::sqrt(norm_a) * std::sqrt(norm_b));
}

void validate_children(const std::vector<ConditionPtr>& children,
                       const char* condition_name) {
    for (const auto& child : children) {
        if (!child) {
            throw std::invalid_argument(std::string(condition_name) +
                                        " condition contains a null child");
        }
    }
}

class AllCondition final : public Condition {
public:
    explicit AllCondition(std::vector<ConditionPtr> children)
        : children_(std::move(children)) {
        if (children_.empty()) {
            throw std::invalid_argument("all condition requires at least one child");
        }
        validate_children(children_, "all");
    }

    bool evaluate(EvalContext& ctx) const override {
        for (const auto& child : children_) {
            if (!child->evaluate(ctx)) return false;
        }
        return true;
    }

private:
    std::vector<ConditionPtr> children_;
};

class AnyCondition final : public Condition {
public:
    explicit AnyCondition(std::vector<ConditionPtr> children)
        : children_(std::move(children)) {
        if (children_.empty()) {
            throw std::invalid_argument("any condition requires at least one child");
        }
        validate_children(children_, "any");
    }

    bool evaluate(EvalContext& ctx) const override {
        for (const auto& child : children_) {
            if (child->evaluate(ctx)) return true;
        }
        return false;
    }

private:
    std::vector<ConditionPtr> children_;
};

class NotCondition final : public Condition {
public:
    explicit NotCondition(ConditionPtr child)
        : child_(std::move(child)) {
        if (!child_) {
            throw std::invalid_argument("not condition requires one child");
        }
    }

    bool evaluate(EvalContext& ctx) const override {
        return !child_->evaluate(ctx);
    }

private:
    ConditionPtr child_;
};

class ClassifierBandCondition final : public Condition {
public:
    ClassifierBandCondition(ClassifierPtr classifier,
                            std::optional<std::string> label,
                            std::optional<double> min_score,
                            std::optional<double> max_score)
        : classifier_(std::move(classifier)),
          label_(std::move(label)),
          min_score_(min_score),
          max_score_(max_score) {
        if (!classifier_) {
            throw std::invalid_argument("classifier condition requires a classifier");
        }
        if (!min_score_.has_value() && !max_score_.has_value()) {
            min_score_ = 0.5;
        }
        if (min_score_.has_value() && max_score_.has_value() && *min_score_ > *max_score_) {
            throw std::invalid_argument("classifier condition min_score cannot exceed max_score");
        }
    }

    bool evaluate(EvalContext& ctx) const override {
        const Score& score = score_for(ctx);

        std::optional<double> traced_score;
        bool result = false;
        if (!score.ok) {
            result = classifier_->on_error() == OnError::MatchTrue;
        } else {
            double value = selected_score(score);
            traced_score = value;
            result = in_band(value);
        }

        if (ctx.want_trace) {
            ctx.trace.push_back(TraceEntry{"classifier:" + classifier_->id(), traced_score, result});
        }
        return result;
    }

private:
    const Score& score_for(EvalContext& ctx) const {
        auto [it, inserted] = ctx.memo.try_emplace(classifier_->id());
        if (inserted) {
            try {
                it->second = classifier_->evaluate(ClassifierContext{ctx.request, ctx.services});
            } catch (const std::exception& e) {
                // Classifier implementations should return Score{ok=false}
                // rather than throw. Keep this catch as a permanent safety
                // net: model-backed services can fail in unexpected ways, and
                // routing must convert that to on_error rather than letting an
                // exception escape evaluation.
                LOG(WARNING, "Routing") << "Classifier '" << classifier_->id()
                                        << "' threw during evaluation: " << e.what()
                                        << std::endl;
                it->second = failed_score();
            } catch (...) {
                LOG(WARNING, "Routing") << "Classifier '" << classifier_->id()
                                        << "' threw an unknown exception during evaluation"
                                        << std::endl;
                it->second = failed_score();
            }
        }
        return it->second;
    }

    double selected_score(const Score& score) const {
        if (label_.has_value()) {
            return score.score_of(*label_);
        }
        if (classifier_->default_label().has_value()) {
            return score.score_of(*classifier_->default_label());
        }
        // Label-less classifiers (no declared labels) return a single score;
        // primary() reads that lone entry.
        return score.primary();
    }

    bool in_band(double value) const {
        if (min_score_.has_value() && value < *min_score_) return false;
        if (max_score_.has_value() && value > *max_score_) return false;
        return true;
    }

    ClassifierPtr classifier_;
    std::optional<std::string> label_;
    std::optional<double> min_score_;
    std::optional<double> max_score_;
};

class ModelClassifier final : public Classifier {
public:
    ModelClassifier(std::string id, std::string type, std::string model, OnError on_error,
                    std::vector<std::string> labels,
                    std::optional<std::string> default_label)
        : Classifier(std::move(id), std::move(type), on_error, std::move(labels),
                     std::move(default_label)),
          model_(std::move(model)) {
        if (model_.empty()) {
            throw std::invalid_argument("classifier requires model");
        }
    }

    Score evaluate(const ClassifierContext& ctx) const override {
        Score score;
        if (!ctx.services.run_classifier) {
            return failed_score();
        }

        try {
            score.labels = ctx.services.run_classifier(model_, ctx.request.input);
            score.ok = true;
        } catch (...) {
            score = failed_score();
        }
        return score;
    }

private:
    std::string model_;
};

class SemanticSimilarityClassifier final : public Classifier {
public:
    // A named concept and its reference phrases. The concept name is the label
    // under which this concept's score is reported, aligning the output with the
    // `classifier` type's label -> score contract.
    using Concept = std::pair<std::string, std::vector<std::string>>;

    // A single embedding vector, the phrase embeddings for one concept, and the
    // per-concept reference embeddings for the whole classifier.
    using Embedding = std::vector<float>;
    using ConceptEmbeddings = std::vector<Embedding>;
    using ReferenceEmbeddings = std::vector<ConceptEmbeddings>;

    SemanticSimilarityClassifier(std::string id, std::string type, std::string model,
                                 std::vector<Concept> concepts, OnError on_error,
                                 std::vector<std::string> labels,
                                 std::optional<std::string> default_label)
        : Classifier(std::move(id), std::move(type), on_error, std::move(labels),
                     std::move(default_label)),
          model_(std::move(model)),
          concepts_(std::move(concepts)) {
        if (model_.empty()) {
            throw std::invalid_argument("semantic_similarity classifier requires model");
        }
        if (concepts_.empty()) {
            throw std::invalid_argument(
                "semantic_similarity classifier requires reference_phrases");
        }
    }

    // Embeds the input once and reports, per concept, the maximum cosine
    // similarity between the input and that concept's reference phrases. The
    // result is a label -> score map (one entry per concept), read back by a
    // condition exactly like a `classifier` score.
    Score evaluate(const ClassifierContext& ctx) const override {
        if (!ctx.services.embed) {
            return failed_score();
        }

        const ReferenceEmbeddings* references = nullptr;
        try {
            references = &reference_embeddings(ctx.services);
        } catch (...) {
            return failed_score();
        }

        Embedding input_embedding;
        try {
            input_embedding = ctx.services.embed(model_, ctx.request.input);
        } catch (...) {
            return failed_score();
        }

        Score score;
        for (std::size_t i = 0; i < concepts_.size(); ++i) {
            // Cosine can be [-1,1] in principle, though embedding vectors are
            // usually non-negative; the running max starts at 0 and the result
            // is clamped into the [0,1] band contract.
            double max_cosine = 0.0;
            for (const auto& reference : (*references)[i]) {
                std::optional<double> cosine = cosine_similarity(input_embedding, reference);
                if (!cosine.has_value()) {
                    return failed_score();
                }
                max_cosine = (std::max)(max_cosine, *cosine);
            }
            score.labels[concepts_[i].first] = std::clamp(max_cosine, 0.0, 1.0);
        }
        score.ok = true;
        return score;
    }

private:
    // Embeds every reference phrase exactly once and caches the vectors on the
    // instance, grouped by concept. Classifiers are shared across concurrent
    // router requests, so the cache is guarded by a mutex.
    //
    // TODO: consider whether this should be done at construction time instead of
    // lazily on the first evaluate() call. The constructor already has the model
    // and reference phrases, so it could embed them immediately. The downside is
    // that it would require the ClassifierServices to be available at
    // construction time, which may not be the case in all contexts.
    const ReferenceEmbeddings& reference_embeddings(
        const ClassifierServices& services) const {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        if (!cached_) {
            ReferenceEmbeddings embeddings;
            embeddings.reserve(concepts_.size());
            for (const auto& concept : concepts_) {
                ConceptEmbeddings phrase_embeddings;
                phrase_embeddings.reserve(concept.second.size());
                for (const auto& phrase : concept.second) {
                    phrase_embeddings.push_back(services.embed(model_, phrase));
                }
                embeddings.push_back(std::move(phrase_embeddings));
            }
            reference_embeddings_ = std::move(embeddings);
            cached_ = true;
        }
        return reference_embeddings_;
    }

    std::string model_;
    std::vector<Concept> concepts_;

    mutable std::mutex cache_mutex_;
    mutable bool cached_ = false;
    mutable ReferenceEmbeddings reference_embeddings_;
};

std::vector<std::string> parse_labels(const json& config, const std::string& id) {
    std::vector<std::string> labels;
    if (config.contains("labels")) {
        if (!config["labels"].is_array()) {
            throw std::invalid_argument("classifier '" + id + "' labels must be an array");
        }
        for (const auto& item : config["labels"]) {
            if (!item.is_string() || item.get<std::string>().empty()) {
                throw std::invalid_argument("classifier '" + id + "' labels must be non-empty strings");
            }
            labels.push_back(item.get<std::string>());
        }
    }
    return labels;
}

std::optional<std::string> parse_default_label(const json& config,
                                               const std::vector<std::string>& labels,
                                               const std::string& id) {
    if (!config.contains("default_label")) {
        return std::nullopt;
    }
    if (!config["default_label"].is_string() || config["default_label"].get<std::string>().empty()) {
        throw std::invalid_argument("classifier '" + id + "' default_label must be a non-empty string");
    }
    std::string default_label = config["default_label"].get<std::string>();
    if (labels.empty()) {
        throw std::invalid_argument("classifier '" + id + "' default_label requires labels");
    }
    if (std::find(labels.begin(), labels.end(), default_label) == labels.end()) {
        throw std::invalid_argument("classifier '" + id + "' default_label is not declared in labels");
    }
    return default_label;
}

std::vector<SemanticSimilarityClassifier::Concept> parse_reference_phrases(
    const json& config, const std::string& id) {
    if (!config.contains("reference_phrases")) {
        throw std::invalid_argument(
            "semantic_similarity classifier '" + id + "' requires reference_phrases");
    }
    const json& reference_phrases = config["reference_phrases"];
    if (!reference_phrases.is_object() || reference_phrases.empty()) {
        throw std::invalid_argument(
            "semantic_similarity classifier '" + id +
            "' reference_phrases must be a non-empty object mapping concept -> phrases");
    }
    std::vector<SemanticSimilarityClassifier::Concept> concepts;
    for (auto it = reference_phrases.begin(); it != reference_phrases.end(); ++it) {
        const std::string& label = it.key();
        if (label.empty()) {
            throw std::invalid_argument(
                "semantic_similarity classifier '" + id + "' has an empty concept name");
        }
        const json& phrases = it.value();
        if (!phrases.is_array() || phrases.empty()) {
            throw std::invalid_argument(
                "semantic_similarity classifier '" + id + "' concept '" + label +
                "' must have a non-empty array of reference phrases");
        }
        std::vector<std::string> phrase_list;
        phrase_list.reserve(phrases.size());
        for (const auto& item : phrases) {
            if (!item.is_string() || item.get<std::string>().empty()) {
                throw std::invalid_argument(
                    "semantic_similarity classifier '" + id + "' concept '" + label +
                    "' reference phrases must be non-empty strings");
            }
            phrase_list.push_back(item.get<std::string>());
        }
        concepts.emplace_back(label, std::move(phrase_list));
    }
    return concepts;
}

std::vector<ConditionPtr> compile_children(const std::vector<MatchExpr>& children,
                                           const LeafFactory& leaf_factory,
                                           std::size_t depth);

ConditionPtr compile_match_expr_impl(const MatchExpr& expr,
                                     const LeafFactory& leaf_factory,
                                     std::size_t depth) {
    if (depth > kMaxMatchExprDepth) {
        throw std::invalid_argument("match expression nesting exceeds maximum depth");
    }

    switch (expr.op) {
        case MatchExpr::Op::Leaf: {
            if (!leaf_factory) {
                throw std::invalid_argument("leaf factory is required");
            }
            auto condition = leaf_factory(expr.leaf);
            if (!condition) {
                throw std::invalid_argument("leaf factory returned null condition");
            }
            return condition;
        }
        case MatchExpr::Op::All:
            if (expr.children.empty()) {
                throw std::invalid_argument("all expression requires at least one child");
            }
            return make_all_condition(compile_children(expr.children, leaf_factory, depth + 1));
        case MatchExpr::Op::Any:
            if (expr.children.empty()) {
                throw std::invalid_argument("any expression requires at least one child");
            }
            return make_any_condition(compile_children(expr.children, leaf_factory, depth + 1));
        case MatchExpr::Op::Not:
            if (expr.children.size() != 1) {
                throw std::invalid_argument("not expression requires exactly one child");
            }
            return make_not_condition(compile_match_expr_impl(expr.children[0], leaf_factory, depth + 1));
    }
    throw std::invalid_argument("unsupported match expression op");
}

std::vector<ConditionPtr> compile_children(const std::vector<MatchExpr>& children,
                                           const LeafFactory& leaf_factory,
                                           std::size_t depth) {
    std::vector<ConditionPtr> compiled;
    compiled.reserve(children.size());
    for (const auto& child : children) {
        compiled.push_back(compile_match_expr_impl(child, leaf_factory, depth));
    }
    return compiled;
}

} // namespace

ConditionPtr make_all_condition(std::vector<ConditionPtr> children) {
    return std::make_shared<AllCondition>(std::move(children));
}

ConditionPtr make_any_condition(std::vector<ConditionPtr> children) {
    return std::make_shared<AnyCondition>(std::move(children));
}

ConditionPtr make_not_condition(ConditionPtr child) {
    return std::make_shared<NotCondition>(std::move(child));
}

ConditionPtr make_classifier_band_condition(ClassifierPtr classifier,
                                            std::optional<std::string> label,
                                            std::optional<double> min_score,
                                            std::optional<double> max_score) {
    return std::make_shared<ClassifierBandCondition>(
        std::move(classifier), std::move(label), min_score, max_score);
}

ConditionPtr compile_match_expr(const MatchExpr& expr, const LeafFactory& leaf_factory) {
    return compile_match_expr_impl(expr, leaf_factory, 0);
}

ClassifierPtr make_classifier(const json& config) {
    if (!config.is_object()) {
        throw std::invalid_argument("classifier entry must be an object");
    }

    const std::string id = config.value("id", "");
    const std::string type = config.value("type", "");
    if (id.empty()) {
        throw std::invalid_argument("classifier is missing id");
    }
    if (type.empty()) {
        throw std::invalid_argument("classifier '" + id + "' is missing type");
    }

    OnError on_error = parse_on_error(config.value("on_error", "match_false"));

    if (type == "classifier") {
        std::vector<std::string> labels = parse_labels(config, id);
        std::optional<std::string> default_label = parse_default_label(config, labels, id);
        return std::make_shared<ModelClassifier>(
            id, type, config.value("model", ""), on_error,
            std::move(labels), std::move(default_label));
    }

    if (type == "semantic_similarity") {
        if (config.contains("labels")) {
            throw std::invalid_argument(
                "semantic_similarity classifier '" + id +
                "' does not accept labels; concept names are the reference_phrases keys");
        }
        std::vector<SemanticSimilarityClassifier::Concept> concepts =
            parse_reference_phrases(config, id);
        std::vector<std::string> labels;
        labels.reserve(concepts.size());
        for (const auto& concept : concepts) {
            labels.push_back(concept.first);
        }
        std::optional<std::string> default_label = parse_default_label(config, labels, id);
        return std::make_shared<SemanticSimilarityClassifier>(
            id, type, config.value("model", ""), std::move(concepts), on_error,
            std::move(labels), std::move(default_label));
    }

    if (type == "llm" ||
        type == "pii_detection" || type == "prompt_safety" ||
        type == "language_detection" || type == "domain_classification" ||
        type == "complexity" || type == "sentiment") {
        throw std::invalid_argument("classifier type not implemented yet: " + type);
    }

    throw std::invalid_argument("unknown classifier type: " + type);
}

std::map<std::string, ClassifierPtr> make_classifiers(const json& classifiers_json) {
    std::map<std::string, ClassifierPtr> classifiers;
    if (classifiers_json.is_null()) {
        return classifiers;
    }
    if (!classifiers_json.is_array()) {
        throw std::invalid_argument("routing.classifiers must be an array");
    }

    for (const auto& item : classifiers_json) {
        auto classifier = make_classifier(item);
        if (!classifiers.emplace(classifier->id(), classifier).second) {
            throw std::invalid_argument("duplicate classifier id: " + classifier->id());
        }
    }
    return classifiers;
}

LeafFactory make_leaf_factory(const std::map<std::string, ClassifierPtr>& classifiers,
                              NamedLeafFactories deterministic_factories) {
    return [classifiers, deterministic_factories = std::move(deterministic_factories)](
               const json& leaf) -> ConditionPtr {
        if (!leaf.is_object()) {
            throw std::invalid_argument("leaf condition must be an object");
        }

        std::vector<ConditionPtr> conditions;
        if (leaf.contains("classifier")) {
            if (!leaf["classifier"].is_string() || leaf["classifier"].get<std::string>().empty()) {
                throw std::invalid_argument("classifier leaf requires non-empty classifier id");
            }
            const std::string id = leaf["classifier"].get<std::string>();
            auto classifier_it = classifiers.find(id);
            if (classifier_it == classifiers.end()) {
                throw std::invalid_argument("unknown classifier reference: " + id);
            }
            const auto& classifier = classifier_it->second;

            std::optional<std::string> label;
            if (leaf.contains("label")) {
                if (!leaf["label"].is_string() || leaf["label"].get<std::string>().empty()) {
                    throw std::invalid_argument("classifier leaf label must be a non-empty string");
                }
                label = leaf["label"].get<std::string>();
                const auto& labels = classifier->labels();
                if (labels.empty() ||
                    std::find(labels.begin(), labels.end(), *label) == labels.end()) {
                    throw std::invalid_argument("classifier '" + id + "' has no label: " + *label);
                }
            } else if (!classifier->labels().empty() && !classifier->default_label().has_value()) {
                throw std::invalid_argument(
                    "classifier leaf omits label but classifier has no default_label: " + id);
            }

            std::optional<double> min_score;
            std::optional<double> max_score;
            if (leaf.contains("min_score")) {
                if (!leaf["min_score"].is_number()) {
                    throw std::invalid_argument("classifier leaf min_score must be numeric");
                }
                min_score = leaf["min_score"].get<double>();
            }
            if (leaf.contains("max_score")) {
                if (!leaf["max_score"].is_number()) {
                    throw std::invalid_argument("classifier leaf max_score must be numeric");
                }
                max_score = leaf["max_score"].get<double>();
            }
            conditions.push_back(
                make_classifier_band_condition(classifier, label, min_score, max_score));
        } else {
            for (const char* classifier_only : {"label", "min_score", "max_score"}) {
                if (leaf.contains(classifier_only)) {
                    throw std::invalid_argument(
                        std::string("classifier leaf field requires classifier: ") + classifier_only);
                }
            }
        }

        for (const auto& [key, factory] : deterministic_factories) {
            if (!leaf.contains(key)) continue;
            if (!factory) {
                throw std::invalid_argument("deterministic leaf factory is null: " + key);
            }
            json single = json::object();
            single[key] = leaf.at(key);
            auto condition = factory(single);
            if (!condition) {
                throw std::invalid_argument("deterministic leaf factory returned null: " + key);
            }
            conditions.push_back(std::move(condition));
        }

        if (conditions.empty()) {
            throw std::invalid_argument("unknown leaf condition");
        }
        if (conditions.size() == 1) {
            return conditions.front();
        }
        return make_all_condition(std::move(conditions));
    };
}

} // namespace lemon
