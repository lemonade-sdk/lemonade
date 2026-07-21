#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>

// Contract surface for the generic routing engine (the "Lemonade Router").
//
// This header is the foundation: the shared types, interfaces, and the engine
// constructor signature that the rest of the engine codes against. It is
// intentionally behavior-free — the match-expression evaluator, the classifier /
// condition registry, the deterministic conditions, the semantic_similarity and
// llm classifiers, the engine assembly, the parser, and the live Router wiring
// all implement against the declarations here.
//
// Design north star: the engine does PURE model selection — boolean rules over
// classifiers, first-match-wins, fail-open to default_model. It emits a Decision
// plus an optional per-condition trace. Everything trust-specific (verdicts,
// block, audit persistence, consent) is layered ON TOP via three seams that the
// engine never interprets:
//   1. RouteContext::metadata   — caller-supplied routing inputs.
//   2. Rule::outputs            — a pass-through bag copied verbatim into Decision.
//   3. Decision::trace          — what the client/audit sink logs.
//
// INVARIANT: this header includes ONLY the standard library and nlohmann/json.
// It must never include a backend or Router header. Backends are reached only
// through ClassifierServices (a struct of std::function injection points), the
// same subprocess-friendly seam pattern used by CollectionOrchestrator.

namespace lemon {

using json = nlohmann::json;

// Maximum nesting for routing match expressions. The parser should enforce this
// at policy load time; compile_match_expr also uses it defensively for manually
// constructed ASTs.
constexpr std::size_t kMaxMatchExprDepth = 64;

// ---------------------------------------------------------------------------
// Request-side context
// ---------------------------------------------------------------------------

// Generic, backend-agnostic view of one routing request. Built by the dispatch
// layer from the inbound OpenAI chat body; consumed by conditions and
// classifiers. No trust vocabulary lives here — `metadata` is an opaque string
// map whose keys are the policy author's business.
struct RouteContext {
    // The text the classifiers/conditions see (typically the latest user turn).
    std::string input;

    // Cheap, deterministic request features. `chars` is a UTF-8 byte count (the
    // frozen v1 unit for min_chars/max_chars; token-based length is deferred to
    // a future min_tokens/max_tokens, never a redefinition of chars).
    struct Params {
        std::string model;          // the collection.router model name addressed
        bool has_tools = false;     // request carried a non-empty tools[] array
        bool has_images = false;    // request carried image content parts
        std::size_t chars = 0;      // UTF-8 byte count of `input`
    } params;

    // Routing inputs carried on the OpenAI `metadata` body field. List values
    // are comma-encoded by the caller; the engine exposes them verbatim. Trust
    // puts keys like "task_class"/"consent" here.
    std::map<std::string, std::string> metadata;
};

// ---------------------------------------------------------------------------
// Classifier output + error policy
// ---------------------------------------------------------------------------

// What a Classifier produces for one (classifier, input) pair. Both `classifier`
// and `semantic_similarity` return label -> score in [0,1]: a `classifier` uses
// the model's labels (HF text-classification convention), while
// `semantic_similarity` reports the max cosine per concept under that concept's
// label. A label-less classifier may instead report a single score under an
// arbitrary key (read via primary()).
//
// Scores are engine-opaque: a condition applies a min_score/max_score band to
// the score of a chosen label to produce a bool.
struct Score {
    // label -> score. For semantic_similarity: one entry per concept.
    std::map<std::string, double> labels;

    // false => the classifier failed to evaluate (model error / timeout); the
    // owning condition then applies its `on_error` policy instead of the band.
    bool ok = true;

    // Optional human-readable rationale (the `llm` router records its pick here)
    // — surfaced in the trace, never used for matching.
    std::string rationale;

    // Score for an explicit label, or 0.0 if absent. Extra labels returned by
    // a classifier are ignored unless a condition references them.
    double score_of(const std::string& label) const {
        auto it = labels.find(label);
        return it == labels.end() ? 0.0 : it->second;
    }

    // The single/primary score — the lone entry of a one-label classifier.
    // Returns 0.0 unless the score has exactly one label, so a condition that
    // omits `label` against a multi-label classifier never silently matches an
    // arbitrary label. Used by classifiers that declare no labels() (their model
    // returns one score and no label name is needed to address it).
    double primary() const {
        return labels.size() == 1 ? labels.begin()->second : 0.0;
    }
};

// Behavior when a classifier fails to evaluate. match_true is "fail-closed
// authoring" — a failed PII/jailbreak check still trips its rule and keeps the
// request local.
enum class OnError {
    MatchTrue,   // "match_true"
    MatchFalse,  // "match_false"
};

// Single source of truth for the on_error string<->enum mapping so the parser
// and any tooling agree. Defaults to MatchFalse (fail-open) when unset or
// unrecognized; the parser is responsible for rejecting bad values loudly.
inline OnError parse_on_error(const std::string& s) {
    return s == "match_true" ? OnError::MatchTrue : OnError::MatchFalse;
}

inline const char* on_error_to_string(OnError e) {
    return e == OnError::MatchTrue ? "match_true" : "match_false";
}

// ---------------------------------------------------------------------------
// Backend injection seam
// ---------------------------------------------------------------------------

// The ONLY way the pure engine touches live backends. Real implementations bind
// these to the Router (embeddings / classifier-model invocation / chat); tests
// bind them to fakes (fixed vectors / fixed scores / fixed text). Keeping them
// std::function keeps routing_policy.h free of any Router include.
struct ClassifierServices {
    // Embed `text` with `model`; powers semantic_similarity. Maps to
    // Router::embeddings.
    std::function<std::vector<float>(const std::string& model,
                                     const std::string& text)> embed;

    // Run a text-classification `model` over `text`; returns label -> score.
    // Powers the generic `classifier` type.
    std::function<std::map<std::string, double>(const std::string& model,
                                                const std::string& text)> run_classifier;

    // Run a chat `model` with a system `prompt` over `input`; returns the raw
    // assistant text. Powers the `llm` router / L0a on-ramp. Maps to
    // Router::chat_completion.
    std::function<std::string(const std::string& model,
                              const std::string& prompt,
                              const std::string& input)> chat;
};

// ---------------------------------------------------------------------------
// Classifiers
// ---------------------------------------------------------------------------

// What a classifier sees when evaluated. Holds the request plus the injected
// services it may call. The const& outlive the call.
struct ClassifierContext {
    const RouteContext& request;
    const ClassifierServices& services;
};

// Abstract base for every classifier type (semantic_similarity, classifier,
// llm, and the reserved vLLM-SR presets). Concrete subclasses and the registry
// that instantiates them from JSON live alongside their implementations.
class Classifier {
public:
    virtual ~Classifier() = default;

    // Evaluate once for the given request. Implementations must set Score::ok
    // false (rather than throw) on backend failure so the owning condition can
    // apply on_error.
    virtual Score evaluate(const ClassifierContext& ctx) const = 0;

    const std::string& id() const { return id_; }
    const std::string& type() const { return type_; }
    OnError on_error() const { return on_error_; }

    // Declared output labels and the optional default. Intrinsic to the
    // declaration, so the registry resolves condition `label` refs against
    // labels() and falls back to default_label() when a condition omits `label`
    // — no sidecar metadata table. For `classifier` these are the model's
    // labels; for `semantic_similarity` they are the concept names (the keys of
    // reference_phrases map).
    // A label-less classifier leaves labels() empty and is read via Score::primary().
    const std::vector<std::string>& labels() const { return labels_; }
    const std::optional<std::string>& default_label() const { return default_label_; }

protected:
    Classifier(std::string id, std::string type, OnError on_error,
               std::vector<std::string> labels = {},
               std::optional<std::string> default_label = std::nullopt)
        : id_(std::move(id)), type_(std::move(type)), on_error_(on_error),
          labels_(std::move(labels)), default_label_(std::move(default_label)) {}

    std::string id_;
    std::string type_;
    OnError on_error_ = OnError::MatchFalse;
    std::vector<std::string> labels_;
    std::optional<std::string> default_label_;
};

using ClassifierPtr = std::shared_ptr<Classifier>;

// ---------------------------------------------------------------------------
// Match AST
// ---------------------------------------------------------------------------

// A node in a rule's `match` expression. Nested any/all/not over leaf
// conditions; vLLM-SR's flat single-operator form is the degenerate subset.
// Leaf condition parsing (deterministic ops, classifier-band refs) is handled
// where those conditions are implemented — the foundation carries the raw leaf
// JSON so the AST shape is fixed without committing to leaf semantics.
struct MatchExpr {
    enum class Op { Leaf, All, Any, Not };

    Op op = Op::Leaf;

    // For All/Any/Not. Not has exactly one child.
    std::vector<MatchExpr> children;

    // For Leaf: the raw condition object, e.g. {"keywords_any":[...]} or
    // {"classifier":"pii","min_score":0.5}.
    json leaf;
};

// ---------------------------------------------------------------------------
// Rules + Decision
// ---------------------------------------------------------------------------

// One ordered, first-match-wins rule. `route_to` must name a candidate.
// `outputs` is engine-opaque and copied verbatim into Decision::outputs.
struct Rule {
    std::string id;
    MatchExpr match;
    std::string route_to;
    json outputs = json::object();
};

// One per-condition trace entry. Emitted only when route_trace=true.
struct TraceEntry {
    std::string condition;          // e.g. "classifier:pii", "keywords_any"
    std::optional<double> score;    // present for classifier conditions
    bool result = false;            // the leaf's boolean outcome
    std::string label;              // optional; the label the band tested (which
                                    // candidate this score belongs to)
    std::string rationale;          // optional; the `llm` router records the
                                    // model's short pick-reason here (empty otherwise)
};

// The engine's output for one request. Pure selection — no verdict /
// route-category / action in core; those are read by trust off `outputs`.
struct Decision {
    std::string route_to;                  // selected candidate (also the `model`)
    std::string matched_rule;              // matched rule id, empty if defaulted
    bool default_used = false;             // true => fell through to default_model
    json outputs = json::object();         // verbatim from the matched rule
    std::vector<TraceEntry> trace;         // populated only when trace requested

    Decision() = default;

    // No rule matched — fail open to default_model. This is the base
    // constructor: it sets route_to, marks default_used, and folds in the trace
    // (gated by want_trace) so a Decision is never built without deciding what
    // happens to its trace. The matched constructor delegates to it.
    Decision(std::string default_model, bool want_trace, std::vector<TraceEntry> trace)
        : route_to(std::move(default_model)), default_used(true) {
        if (want_trace) this->trace = std::move(trace);
    }

    // A rule matched: delegate to the default constructor for the route_to +
    // trace plumbing, then flip default_used off and layer on the matched rule's
    // id and outputs.
    Decision(const Rule& rule, bool want_trace, std::vector<TraceEntry> trace)
        : Decision(rule.route_to, want_trace, std::move(trace)) {
        default_used = false;
        matched_rule = rule.id;
        outputs = rule.outputs;
    }
};

// ---------------------------------------------------------------------------
// Match evaluation (runtime tree)
// ---------------------------------------------------------------------------

// Per-request state threaded through a Condition tree during evaluation.
// Mutable: classifiers memoize their Score here (each runs at most once per
// request) and leaves append to `trace` when `want_trace` is set.
struct EvalContext {
    const RouteContext& request;
    const ClassifierServices& services;
    bool want_trace = false;

    // classifier id -> its Score for this request. Input text is constant within
    // a request, so the classifier id is a sufficient memo key.
    std::map<std::string, Score> memo;

    // Per-condition trace; appended only when want_trace, surfaced verbatim as
    // Decision::trace.
    std::vector<TraceEntry> trace;

    // ASCII-lowered copy of request.input, memoized so keyword leaves fold the
    // input at most once per request (the input is constant within a request).
    std::optional<std::string> lowered_input;
};

// A node in a rule's compiled match tree. Both composites (all/any/not) and
// leaves (deterministic ops, classifier-band) are Conditions. The registry
// compiles a MatchExpr — whose leaves are raw JSON — into a Condition tree via a
// LeafFactory; the evaluator supplies the composites and the classifier-band
// leaf; the engine evaluates the root Condition per rule.
//
// Implementations MUST NOT throw: a classifier failure surfaces via Score::ok
// and the band's on_error policy, never an exception (the engine fails open to
// default_model on anything unexpected).
class Condition {
public:
    virtual ~Condition() = default;
    virtual bool evaluate(EvalContext& ctx) const = 0;
};
using ConditionPtr = std::shared_ptr<Condition>;

// Builds a leaf Condition from a single leaf object (e.g. {"keywords_any":[...]}
// or {"classifier":"pii","min_score":0.5}). This is the seam between the
// structural evaluator (composites) and the concrete leaf builders (deterministic
// ops + classifier-band) registered downstream — neither side depends on the
// other's implementation, only on this typedef.
using LeafFactory = std::function<ConditionPtr(const json& leaf)>;
using NamedLeafFactories = std::map<std::string, LeafFactory>;

// Composite evaluator nodes and compiler (#2378). These are pure structural
// conditions; concrete leaf behavior is supplied through LeafFactory.
ConditionPtr make_all_condition(std::vector<ConditionPtr> children);
ConditionPtr make_any_condition(std::vector<ConditionPtr> children);
ConditionPtr make_not_condition(ConditionPtr child);

// Classifier-band leaf (#2378). Applies an inclusive score band to a resolved
// Classifier's Score, memoizing classifier execution in EvalContext.
ConditionPtr make_classifier_band_condition(ClassifierPtr classifier,
                                            std::optional<std::string> label,
                                            std::optional<double> min_score,
                                            std::optional<double> max_score);

// Compile a parsed MatchExpr into an executable Condition tree. Composite nodes
// are built here; leaf nodes delegate to the injected factory so deterministic
// ops and classifier-band leaves can be registered independently.
ConditionPtr compile_match_expr(const MatchExpr& expr, const LeafFactory& leaf_factory);

// Classifier / condition registry helpers (#2379). These instantiate the
// behavior-free contract objects from policy JSON while keeping live backend
// access behind ClassifierServices.
ClassifierPtr make_classifier(const json& config);
std::map<std::string, ClassifierPtr> make_classifiers(const json& classifiers_json);

// Builds the leaf factory used by compile_match_expr. Classifier leaves are
// resolved here; deterministic leaf types are supplied by later issues.
LeafFactory make_leaf_factory(const std::map<std::string, ClassifierPtr>& classifiers,
                              NamedLeafFactories deterministic_factories = {});

// Deterministic leaf conditions (#2380): keywords_any/keywords_all, regex,
// min_chars/max_chars, has_tools/has_images, metadata. Pure CPU, no model, no
// tokenizer; each implements the frozen v1 semantics pinned in
// route_policy.schema.json. Pass the result as make_leaf_factory's
// deterministic_factories so rules can use these ops.
NamedLeafFactories make_deterministic_leaf_factories();

// ---------------------------------------------------------------------------
// Policy + engine (constructor signature only here)
// ---------------------------------------------------------------------------

// The parsed, resolved routing policy (produced by the parser). Classifier
// condition refs in the rules resolve against `classifiers` by id.
struct RoutePolicy {
    std::vector<std::string> candidates;                 // routing targets
    std::string default_model;                           // fail-open target ∈ candidates
    std::vector<Rule> rules;                             // ordered, first-match-wins
    std::map<std::string, ClassifierPtr> classifiers;    // id -> classifier
};

// The routing engine. The CONSTRUCTOR SIGNATURE is frozen here. The constructor
// compiles every rule's MatchExpr into an immutable Condition tree (via
// compile_match_expr + the classifier/deterministic LeafFactory); route()
// walks those trees first-match-wins and fails open to default_model.
//
// The engine is const after construction and therefore safe to share across
// concurrent requests: all per-request mutable state lives in a local
// EvalContext. Hot-reload (#2383) swaps the whole engine behind a shared_ptr
// rather than mutating one in place.
class RoutingPolicyEngine {
public:
    RoutingPolicyEngine(RoutePolicy policy, ClassifierServices services);

    // Select a candidate for `ctx`. When `want_trace` is set, the returned
    // Decision carries a per-condition trace. Never throws: any unexpected
    // failure fails open to default_model with default_used=true.
    Decision route(const RouteContext& ctx, bool want_trace) const;

    const RoutePolicy& policy() const { return policy_; }

private:
    RoutePolicy policy_;
    ClassifierServices services_;
    // Compiled match tree per rule, parallel to policy_.rules. Immutable after
    // construction; each ConditionPtr is itself a shared_ptr to const-usable
    // state.
    std::vector<ConditionPtr> compiled_rules_;
};

} // namespace lemon
