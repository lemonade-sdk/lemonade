#include "lemon/routing_policy.h"

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <lemon/utils/aixlog.hpp>

namespace lemon {
namespace {

Score failed_score() {
    Score score;
    score.ok = false;
    return score;
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
        // Single-score classifiers, e.g. semantic_similarity, declare no labels
        // and report their score under the empty-string key.
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

    std::optional<std::string> default_label;
    if (config.contains("default_label")) {
        if (!config["default_label"].is_string() || config["default_label"].get<std::string>().empty()) {
            throw std::invalid_argument("classifier '" + id + "' default_label must be a non-empty string");
        }
        default_label = config["default_label"].get<std::string>();
        if (labels.empty()) {
            throw std::invalid_argument("classifier '" + id + "' default_label requires labels");
        }
        if (std::find(labels.begin(), labels.end(), *default_label) == labels.end()) {
            throw std::invalid_argument("classifier '" + id + "' default_label is not declared in labels");
        }
    }

    if (type == "classifier") {
        return std::make_shared<ModelClassifier>(
            id, type, config.value("model", ""), on_error, std::move(labels), std::move(default_label));
    }

    if (type == "semantic_similarity" || type == "llm" ||
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
