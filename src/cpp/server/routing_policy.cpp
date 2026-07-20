#include "lemon/routing_policy.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <mutex>
#include <regex>
#include <set>
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

// Trim leading/trailing ASCII whitespace. Used by the llm router to normalize a
// chat reply before matching it against candidate names.
std::string trim_copy(const std::string& s) {
    const char* ws = " \t\r\n";
    const auto begin = s.find_first_not_of(ws);
    if (begin == std::string::npos) return std::string();
    const auto end = s.find_last_not_of(ws);
    return s.substr(begin, end - begin + 1);
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
            TraceEntry entry{"classifier:" + classifier_->id(), traced_score, result};
            if (label_.has_value()) {
                entry.label = *label_;
            }
            // The rationale explains the labels the classifier actually scored
            // (for the llm router: the single selected candidate). A band that
            // tests a different label must not carry it — otherwise the trace
            // attaches the winner's rationale to rejected candidates.
            const bool rationale_applies =
                !label_.has_value() || score.labels.count(*label_) > 0;
            if (score.ok && !score.rationale.empty() && rationale_applies) {
                entry.rationale = score.rationale;
            }
            ctx.trace.push_back(std::move(entry));
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

// The `llm` router / L0(a) on-ramp. Runs a small chat model with the author's
// prompt plus a fixed response-contract suffix (listing the candidates), and
// requires a structured reply: a single JSON object
//   {"model": "<exact candidate name>", "rationale": "<one short sentence>"}
// The chosen candidate becomes the label (score 1.0) and the model's stated
// rationale is recorded in Score::rationale for the trace. Malformed JSON, a
// missing/unknown "model", or any extra prose yields an empty Score (ok=true,
// no labels): no identity rule fires and the engine fails open to
// default_model. A backend failure yields Score{ok=false} so the owning
// condition applies on_error.
class LlmClassifier final : public Classifier {
public:
    LlmClassifier(std::string id, std::string type, std::string model, std::string prompt,
                  OnError on_error, std::vector<std::string> labels,
                  std::optional<std::string> default_label)
        : Classifier(std::move(id), std::move(type), on_error, std::move(labels),
                     std::move(default_label)),
          model_(std::move(model)), prompt_(std::move(prompt)) {
        if (model_.empty()) {
            throw std::invalid_argument("llm classifier requires model");
        }
        if (prompt_.empty()) {
            throw std::invalid_argument("llm classifier requires prompt");
        }
        if (this->labels().empty()) {
            throw std::invalid_argument("llm classifier requires at least one label (candidate)");
        }
    }

    Score evaluate(const ClassifierContext& ctx) const override {
        if (!ctx.services.chat) {
            return failed_score();
        }
        std::string reply;
        try {
            reply = ctx.services.chat(model_, effective_prompt(),
                                      build_context_payload(ctx.request));
        } catch (...) {
            return failed_score();
        }

        Score score;
        score.ok = true;

        const json parsed = parse_structured_reply(reply);
        if (!parsed.is_object() || parsed.size() != 2 ||
            !parsed.contains("model") || !parsed["model"].is_string() ||
            !parsed.contains("rationale") || !parsed["rationale"].is_string()) {
            // The backend only guarantees a JSON object. Lemonade owns the
            // portable protocol contract: exactly `model` and `rationale`, no
            // missing fields and no backend/model-invented extras. Invalid
            // replies fail open to default_model.
            return score;
        }
        const std::string rationale = trim_copy(parsed["rationale"].get<std::string>());
        if (rationale.empty()) {
            return score;
        }
        const std::string chosen = parsed["model"].get<std::string>();
        const std::string* label = match_label(chosen);
        if (label == nullptr) {
            // Unknown choice => same fail-open path.
            return score;
        }
        score.labels[*label] = 1.0;
        score.rationale = rationale;
        return score;
    }

private:
    // The user message handed to the router LLM: a structured JSON view of the
    // routing context, so the router can see everything a deterministic rule
    // could — not just the latest text. An image-only request is visible as
    // has_images=true with empty text; a tool-bearing request as
    // has_tools=true; length via chars; caller routing hints via metadata.
    //
    // History policy (explicit, per the frozen v1 RouteContext contract):
    // `text` is the LATEST USER TURN ONLY. RouteContext deliberately carries a
    // single turn so earlier assistant/system turns can't skew routing;
    // supplying more history to the router requires extending that contract
    // for all classifiers, which is out of scope for this classifier.
    static std::string build_context_payload(const RouteContext& request) {
        json metadata = json::object();
        for (const auto& [key, value] : request.metadata) {
            metadata[key] = value;
        }
        const json payload = {
            {"text", request.input},
            {"has_tools", request.params.has_tools},
            {"has_images", request.params.has_images},
            {"chars", request.params.chars},
            {"metadata", std::move(metadata)},
        };
        return payload.dump();
    }

    // The author's prompt describes routing intent; the response contract —
    // input format, candidate vocabulary, and the required JSON shape — is
    // appended here so every llm router speaks the same protocol regardless
    // of authoring.
    std::string effective_prompt() const {
        std::string suffix =
            "The user message is a JSON object describing the request to "
            "route: {\"text\": latest user turn only, \"has_tools\": whether "
            "the request carries tools, \"has_images\": whether it carries "
            "images, \"chars\": byte length of text, \"metadata\": caller "
            "routing hints}. You must choose exactly one of these models: ";
        for (std::size_t i = 0; i < labels().size(); ++i) {
            if (i > 0) suffix += ", ";
            suffix += labels()[i];
        }
        suffix +=
            ". Respond with ONLY a JSON object of the form "
            "{\"model\": \"<one of the listed names>\", "
            "\"rationale\": \"<one short sentence>\"} and no other text.";
        return prompt_ + "\n\n" + suffix;
    }

    // Parse the model's reply into the structured-choice object. Strict: the
    // trimmed reply must be a JSON object. One leading/trailing ``` fence pair
    // is tolerated and stripped (instruction-tuned models often add it), but
    // the closing fence must END the reply — trailing prose after it violates
    // the "no other text" contract and rejects.
    static json parse_structured_reply(const std::string& reply) {
        std::string text = trim_copy(reply);
        if (text.rfind("```", 0) == 0) {
            const auto first_newline = text.find('\n');
            const auto last_fence = text.rfind("```");
            if (first_newline == std::string::npos || last_fence <= first_newline) {
                return json(nullptr);  // opening fence without a closing fence
            }
            if (!trim_copy(text.substr(last_fence + 3)).empty()) {
                return json(nullptr);  // text after the closing fence
            }
            text = trim_copy(text.substr(first_newline + 1, last_fence - first_newline - 1));
        }
        json parsed = json::parse(text, nullptr, /*allow_exceptions=*/false);
        return parsed.is_discarded() ? json(nullptr) : parsed;
    }

    // Which candidate did the model name? Exact match only, against the
    // structured "model" field. A substring fallback was rejected in review:
    // it can turn contradictory or negated answers into a valid route and
    // weakens the required invalid-choice fallback.
    const std::string* match_label(const std::string& chosen) const {
        for (const auto& label : labels()) {
            if (chosen == label) return &label;
        }
        return nullptr;
    }

    std::string model_;
    std::string prompt_;
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

// ---------------------------------------------------------------------------
// Deterministic leaf conditions (#2380)
//
// Pure CPU, no model, no tokenizer. Each implements one frozen v1 op exactly as
// pinned in route_policy.schema.json. Config is validated when the condition is
// built (the factory throws std::invalid_argument on malformed leaves); evaluate()
// never throws, so the engine's fail-open contract holds.
// ---------------------------------------------------------------------------

void trace_leaf(EvalContext& ctx, const char* name, bool result) {
    if (ctx.want_trace) {
        ctx.trace.push_back(TraceEntry{name, std::nullopt, result});
    }
}

// Frozen v1: case-insensitivity is an ASCII-only fold (locale-independent, no
// Unicode/ICU dependency). Bytes outside A-Z are left untouched.
std::string ascii_lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return s;
}

bool is_ascii_ws(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// Frozen v1: a metadata value is "present" only if it carries at least one
// non-whitespace character. A missing, empty, or whitespace-only value is
// treated as absent (matches only exists:false), keeping exists consistent with
// the comma-split token model used by equals/any.
bool is_blank(const std::string& s) {
    for (char c : s) {
        if (!is_ascii_ws(c)) return false;
    }
    return true;
}

// Frozen v1 metadata decode: split on comma, trim ASCII whitespace from each
// token, drop empties. Scalar and comma-encoded list values decode uniformly.
std::set<std::string> decode_metadata_tokens(const std::string& value) {
    std::set<std::string> tokens;
    std::size_t start = 0;
    while (true) {
        std::size_t comma = value.find(',', start);
        std::size_t end = (comma == std::string::npos) ? value.size() : comma;
        std::size_t a = start;
        std::size_t b = end;
        while (a < b && is_ascii_ws(value[a])) ++a;
        while (b > a && is_ascii_ws(value[b - 1])) --b;
        if (b > a) {
            tokens.insert(value.substr(a, b - a));
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return tokens;
}

// keywords_any / keywords_all — case-insensitive substring over the input text.
class KeywordsCondition final : public Condition {
public:
    KeywordsCondition(std::vector<std::string> keywords, bool require_all)
        : require_all_(require_all) {
        keywords_lower_.reserve(keywords.size());
        for (auto& k : keywords) {
            keywords_lower_.push_back(ascii_lower(std::move(k)));
        }
    }

    bool evaluate(EvalContext& ctx) const override {
        // Fold the input at most once per request — it is constant within a
        // request and several keyword leaves may share it.
        if (!ctx.lowered_input.has_value()) {
            ctx.lowered_input = ascii_lower(ctx.request.input);
        }
        const std::string& haystack = *ctx.lowered_input;
        bool result = require_all_;
        for (const auto& needle : keywords_lower_) {
            const bool present = haystack.find(needle) != std::string::npos;
            if (require_all_ && !present) {
                result = false;
                break;
            }
            if (!require_all_ && present) {
                result = true;
                break;
            }
        }
        trace_leaf(ctx, require_all_ ? "keywords_all" : "keywords_any", result);
        return result;
    }

private:
    std::vector<std::string> keywords_lower_;
    bool require_all_;
};

// regex — ECMAScript dialect (std::regex), matched against the input text.
class RegexCondition final : public Condition {
public:
    explicit RegexCondition(const std::string& pattern)
        : regex_(pattern, std::regex::ECMAScript) {}

    bool evaluate(EvalContext& ctx) const override {
        // Even a well-formed pattern can take pathologically long on a very large
        // input. The load-time reject_catastrophic_regex check cannot bound that,
        // so cap the input fed to the engine: an input above the cap is treated as
        // a non-match (fail-safe) rather than risking a hung worker thread.
        constexpr std::size_t kRegexMaxInputBytes = 1u << 20;  // 1 MiB
        if (ctx.request.input.size() > kRegexMaxInputBytes) {
            trace_leaf(ctx, "regex", false);
            return false;
        }

        bool result = false;
        try {
            result = std::regex_search(ctx.request.input, regex_);
        } catch (const std::regex_error&) {
            // Catastrophic backtracking / stack overflow: fail the leaf rather
            // than let an exception escape evaluate(). std::regex's step limit is
            // implementation-defined (MSVC throws error_complexity here; libstdc++
            // / libc++ may not), so this is defense-in-depth behind the load-time
            // reject_catastrophic_regex check, not the primary guard.
            result = false;
        }
        trace_leaf(ctx, "regex", result);
        return result;
    }

private:
    std::regex regex_;
};

// Reject regex patterns prone to catastrophic backtracking (ReDoS) at policy
// load: an unbounded quantifier applied to a group that itself contains an
// unbounded quantifier — the (a+)+, (a*)*, (.*)+, (\d+){2,} family. std::regex
// is a backtracking engine with no portable step limit, and the matched input
// is untrusted end-user text, so a careless policy pattern is a denial-of-service
// vector. This catches the common nested-quantifier shapes before they reach the
// hot path; it is a mitigation, not a proof of linear time — alternation-overlap
// ReDoS such as (a|a)+ is not detected. A hard linear-time guarantee would need a
// non-backtracking engine (RE2), which — being a different dialect — would ship
// as a separately named op, never a redefinition of this ECMAScript one.
void reject_catastrophic_regex(const std::string& pattern) {
    struct GroupState {
        bool has_unbounded = false;  // an unbounded quantifier appears inside
    };
    std::vector<GroupState> stack;
    bool prev_closed_group = false;       // previous token closed a group
    bool closed_group_unbounded = false;  // ...and that group held an unbounded quantifier

    for (std::size_t i = 0; i < pattern.size(); ++i) {
        const char c = pattern[i];
        if (c == '\\') {  // escaped metacharacter — consume the next char literally
            ++i;
            prev_closed_group = false;
            continue;
        }
        if (c == '[') {  // character class — quantifier metacharacters are literal inside
            ++i;
            if (i < pattern.size() && pattern[i] == '^') ++i;
            if (i < pattern.size() && pattern[i] == ']') ++i;  // leading ] is literal
            while (i < pattern.size() && pattern[i] != ']') {
                if (pattern[i] == '\\') ++i;
                ++i;
            }
            prev_closed_group = false;
            continue;
        }
        if (c == '(') {
            stack.push_back(GroupState{});
            prev_closed_group = false;
            continue;
        }
        if (c == ')') {
            closed_group_unbounded = !stack.empty() && stack.back().has_unbounded;
            if (!stack.empty()) stack.pop_back();
            // A wrapper group must not hide its child's unbounded quantifier: an
            // unbounded inner group makes the enclosing group unbounded too, so a
            // later quantifier on the wrapper (e.g. ((a+))+) is still caught.
            if (closed_group_unbounded && !stack.empty()) {
                stack.back().has_unbounded = true;
            }
            prev_closed_group = true;
            continue;
        }

        bool unbounded_quant = false;
        if (c == '*' || c == '+') {
            unbounded_quant = true;
        } else if (c == '{') {  // {m,} is unbounded; {m} and {m,n} are bounded
            std::size_t j = i + 1;
            std::string inner;
            while (j < pattern.size() && pattern[j] != '}') inner += pattern[j++];
            const std::size_t comma = inner.find(',');
            if (comma != std::string::npos && comma + 1 >= inner.size()) {
                unbounded_quant = true;
            }
            i = j;  // advance past the brace expression (loop's ++i steps over '}')
        }

        if (unbounded_quant) {
            if (prev_closed_group && closed_group_unbounded) {
                throw std::invalid_argument(
                    "regex pattern is prone to catastrophic backtracking (nested "
                    "unbounded quantifier such as (X+)+); rewrite to avoid the nesting");
            }
            if (!stack.empty()) stack.back().has_unbounded = true;
        }
        prev_closed_group = false;
    }
}

// min_chars / max_chars — inclusive bound on input length in UTF-8 bytes.
class CharsCondition final : public Condition {
public:
    enum class Bound { Min, Max };

    CharsCondition(std::size_t threshold, Bound bound)
        : threshold_(threshold), bound_(bound) {}

    bool evaluate(EvalContext& ctx) const override {
        const std::size_t n = ctx.request.params.chars;
        const bool result =
            bound_ == Bound::Min ? (n >= threshold_) : (n <= threshold_);
        trace_leaf(ctx, bound_ == Bound::Min ? "min_chars" : "max_chars", result);
        return result;
    }

private:
    std::size_t threshold_;
    Bound bound_;
};

// has_tools / has_images — boolean request feature equals the authored value.
class BoolFeatureCondition final : public Condition {
public:
    enum class Feature { Tools, Images };

    BoolFeatureCondition(Feature feature, bool expected)
        : feature_(feature), expected_(expected) {}

    bool evaluate(EvalContext& ctx) const override {
        const bool actual = feature_ == Feature::Tools ? ctx.request.params.has_tools
                                                       : ctx.request.params.has_images;
        const bool result = actual == expected_;
        trace_leaf(ctx, feature_ == Feature::Tools ? "has_tools" : "has_images", result);
        return result;
    }

private:
    Feature feature_;
    bool expected_;
};

// metadata — match a caller-supplied metadata key via exactly one comparator.
// Frozen v1: case-sensitive; a missing/empty value matches only exists:false.
class MetadataCondition final : public Condition {
public:
    enum class Comparator { Equals, Any, Exists };

    MetadataCondition(std::string key, Comparator cmp, std::string equals_value,
                      std::set<std::string> any_tokens, bool exists_expected)
        : key_(std::move(key)), cmp_(cmp), equals_value_(std::move(equals_value)),
          any_tokens_(std::move(any_tokens)), exists_expected_(exists_expected) {}

    bool evaluate(EvalContext& ctx) const override {
        auto it = ctx.request.metadata.find(key_);
        const bool present = it != ctx.request.metadata.end() && !is_blank(it->second);
        bool result = false;
        switch (cmp_) {
            case Comparator::Exists:
                result = (present == exists_expected_);
                break;
            case Comparator::Equals:
                result = present && it->second == equals_value_;
                break;
            case Comparator::Any:
                if (present) {
                    for (const auto& token : decode_metadata_tokens(it->second)) {
                        if (any_tokens_.count(token) != 0) {
                            result = true;
                            break;
                        }
                    }
                }
                break;
        }
        trace_leaf(ctx, "metadata", result);
        return result;
    }

private:
    std::string key_;
    Comparator cmp_;
    std::string equals_value_;
    std::set<std::string> any_tokens_;
    bool exists_expected_;
};

// -- factory-side validation helpers ----------------------------------------

ConditionPtr build_keywords(const json& arr, bool require_all, const char* op) {
    if (!arr.is_array() || arr.empty()) {
        throw std::invalid_argument(std::string(op) +
                                    " requires a non-empty array of strings");
    }
    std::vector<std::string> keywords;
    keywords.reserve(arr.size());
    for (const auto& item : arr) {
        if (!item.is_string() || item.get<std::string>().empty()) {
            throw std::invalid_argument(std::string(op) +
                                        " items must be non-empty strings");
        }
        keywords.push_back(item.get<std::string>());
    }
    return std::make_shared<KeywordsCondition>(std::move(keywords), require_all);
}

ConditionPtr build_chars(const json& value, CharsCondition::Bound bound, const char* op) {
    if (!value.is_number_integer() || value.get<long long>() < 0) {
        throw std::invalid_argument(std::string(op) +
                                    " requires a non-negative integer");
    }
    return std::make_shared<CharsCondition>(
        static_cast<std::size_t>(value.get<long long>()), bound);
}

ConditionPtr build_bool_feature(const json& value, BoolFeatureCondition::Feature feature,
                                const char* op) {
    if (!value.is_boolean()) {
        throw std::invalid_argument(std::string(op) + " requires a boolean");
    }
    return std::make_shared<BoolFeatureCondition>(feature, value.get<bool>());
}

ConditionPtr build_metadata(const json& spec) {
    if (!spec.is_object()) {
        throw std::invalid_argument("metadata requires an object");
    }
    if (!spec.contains("key") || !spec["key"].is_string() ||
        spec["key"].get<std::string>().empty()) {
        throw std::invalid_argument("metadata requires a non-empty string 'key'");
    }
    const std::string key = spec["key"].get<std::string>();

    const int comparators = static_cast<int>(spec.contains("equals")) +
                            static_cast<int>(spec.contains("any")) +
                            static_cast<int>(spec.contains("exists"));
    if (comparators != 1) {
        throw std::invalid_argument(
            "metadata requires exactly one comparator (equals/any/exists)");
    }

    if (spec.contains("equals")) {
        if (!spec["equals"].is_string()) {
            throw std::invalid_argument("metadata 'equals' must be a string");
        }
        return std::make_shared<MetadataCondition>(
            key, MetadataCondition::Comparator::Equals, spec["equals"].get<std::string>(),
            std::set<std::string>{}, false);
    }
    if (spec.contains("any")) {
        const json& arr = spec["any"];
        if (!arr.is_array() || arr.empty()) {
            throw std::invalid_argument(
                "metadata 'any' must be a non-empty array of strings");
        }
        std::set<std::string> tokens;
        for (const auto& item : arr) {
            if (!item.is_string() || item.get<std::string>().empty()) {
                throw std::invalid_argument(
                    "metadata 'any' items must be non-empty strings");
            }
            tokens.insert(item.get<std::string>());
        }
        return std::make_shared<MetadataCondition>(
            key, MetadataCondition::Comparator::Any, std::string{}, std::move(tokens),
            false);
    }
    if (!spec["exists"].is_boolean()) {
        throw std::invalid_argument("metadata 'exists' must be a boolean");
    }
    return std::make_shared<MetadataCondition>(
        key, MetadataCondition::Comparator::Exists, std::string{},
        std::set<std::string>{}, spec["exists"].get<bool>());
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

    if (type == "llm") {
        std::vector<std::string> labels = parse_labels(config, id);
        std::optional<std::string> default_label = parse_default_label(config, labels, id);
        return std::make_shared<LlmClassifier>(
            id, type, config.value("model", ""), config.value("prompt", ""),
            on_error, std::move(labels), std::move(default_label));
    }

    if (type == "pii_detection" || type == "prompt_safety" ||
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

NamedLeafFactories make_deterministic_leaf_factories() {
    NamedLeafFactories factories;

    factories["keywords_any"] = [](const json& leaf) -> ConditionPtr {
        return build_keywords(leaf.at("keywords_any"), /*require_all=*/false, "keywords_any");
    };
    factories["keywords_all"] = [](const json& leaf) -> ConditionPtr {
        return build_keywords(leaf.at("keywords_all"), /*require_all=*/true, "keywords_all");
    };
    factories["regex"] = [](const json& leaf) -> ConditionPtr {
        const json& value = leaf.at("regex");
        if (!value.is_string()) {
            throw std::invalid_argument("regex requires a string pattern");
        }
        const std::string pattern = value.get<std::string>();
        if (pattern.empty()) {
            throw std::invalid_argument(
                "regex pattern must be non-empty (an empty pattern matches everything)");
        }
        reject_catastrophic_regex(pattern);
        try {
            return std::make_shared<RegexCondition>(pattern);
        } catch (const std::regex_error& e) {
            throw std::invalid_argument(
                std::string("regex pattern is not valid ECMAScript: ") + e.what());
        }
    };
    factories["min_chars"] = [](const json& leaf) -> ConditionPtr {
        return build_chars(leaf.at("min_chars"), CharsCondition::Bound::Min, "min_chars");
    };
    factories["max_chars"] = [](const json& leaf) -> ConditionPtr {
        return build_chars(leaf.at("max_chars"), CharsCondition::Bound::Max, "max_chars");
    };
    factories["has_tools"] = [](const json& leaf) -> ConditionPtr {
        return build_bool_feature(leaf.at("has_tools"), BoolFeatureCondition::Feature::Tools,
                                  "has_tools");
    };
    factories["has_images"] = [](const json& leaf) -> ConditionPtr {
        return build_bool_feature(leaf.at("has_images"), BoolFeatureCondition::Feature::Images,
                                  "has_images");
    };
    factories["metadata"] = [](const json& leaf) -> ConditionPtr {
        return build_metadata(leaf.at("metadata"));
    };

    return factories;
}

RoutingPolicyEngine::RoutingPolicyEngine(RoutePolicy policy, ClassifierServices services)
    : policy_(std::move(policy)), services_(std::move(services)) {
    // Compile every rule's match expression once, at construction, so route()
    // does pure tree-walking on immutable state. Classifier leaves resolve
    // against policy_.classifiers; deterministic leaf types (keywords/regex/
    // min_chars/has_*/metadata) plug in via make_deterministic_leaf_factories().
    LeafFactory leaf_factory =
        make_leaf_factory(policy_.classifiers, make_deterministic_leaf_factories());
    compiled_rules_.reserve(policy_.rules.size());
    for (const auto& rule : policy_.rules) {
        compiled_rules_.push_back(compile_match_expr(rule.match, leaf_factory));
    }
}

Decision RoutingPolicyEngine::route(const RouteContext& ctx, bool want_trace) const {
    EvalContext eval{ctx, services_, want_trace, {}, {}, {}};

    // First-match-wins over the compiled rules. A classifier-level failure is
    // already absorbed upstream by ClassifierBandCondition (Score::ok + the
    // band's on_error), so this catch is only a last-resort guard: any
    // unexpected throw fails the whole request open to default_model rather than
    // escaping route().
    try {
        for (std::size_t i = 0; i < compiled_rules_.size(); ++i) {
            if (compiled_rules_[i]->evaluate(eval)) {
                return Decision(policy_.rules[i], want_trace, std::move(eval.trace));
            }
        }
    } catch (const std::exception& e) {
        LOG(WARNING, "Routing") << "Routing policy evaluation threw; failing open to '"
                                << policy_.default_model << "': " << e.what() << std::endl;
    } catch (...) {
        LOG(WARNING, "Routing") << "Routing policy evaluation threw an unknown exception; "
                                << "failing open to '" << policy_.default_model << "'"
                                << std::endl;
    }

    return Decision(policy_.default_model, want_trace, std::move(eval.trace));
}

} // namespace lemon
