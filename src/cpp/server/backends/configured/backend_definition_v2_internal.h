#pragma once

#include "lemon/backends/configured/backend_definition_resolver.h"

#include <functional>
#include <map>
#include <stdexcept>

namespace lemon::backends::v2 {

const nlohmann::json& schema_document();
CompositionDiagnostics check_schema_keywords(const nlohmann::json& schema);
std::string pointer_token(std::string_view token);

struct SourceRecord {
    DefinitionKey key;
    std::string bytes;
    nlohmann::json header;
    std::size_t nodes = 0;
};

std::string key_string(const DefinitionKey& key);
CompositionDiagnostics preflight_source(const SourceRecord& source, const DefinitionLimits& limits);
using Json = nlohmann::json;

struct LimitExceeded : std::runtime_error {
    LimitExceeded() : std::runtime_error("Composition budget exceeded") {}
};

struct AdmissionCounters {
    std::size_t dom = 0;
    std::size_t clones = 0;
    std::size_t origins = 0;
    std::size_t pointers = 0;
};
extern thread_local AdmissionCounters admission_counters;

void charge(std::size_t& used, std::size_t increment, std::size_t limit);
std::size_t checked_sum(std::size_t left, std::size_t right);
std::size_t checked_product(std::size_t left, std::size_t right);
std::size_t string_bytes(std::string_view value);

struct Metrics {
    std::size_t bytes = 0;
    std::size_t nodes = 0;
    std::size_t depth = 0;
};

struct Budget {
    const DefinitionLimits& limits;
    ResolutionMetrics metrics;
    std::size_t& retained;
    std::map<std::string, std::shared_ptr<const std::string>> segments;
    void work(std::size_t bytes, std::size_t nodes);
    void keep(std::size_t bytes);
    void admit(const Metrics& value);
};

struct Origin;
using OriginPtr = std::shared_ptr<const Origin>;
struct Origin {
    std::shared_ptr<const DefinitionKey> document;
    OriginPtr parent;
    std::shared_ptr<const std::string> token;
};

struct Node;
using NodePtr = std::shared_ptr<const Node>;
struct Node {
    Json scalar;
    std::map<std::string, NodePtr> object;
    std::vector<NodePtr> array;
    enum class Kind { Scalar, Object, Array } kind = Kind::Scalar;
    Metrics metrics;
    OriginPtr origin;
};

OriginPtr origin_child(const OriginPtr& parent, std::string_view token, Budget& budget);
SourceLocation location(const OriginPtr& origin);
Metrics measure(const Json& value, Budget& budget);
NodePtr authored(const Json& value, const OriginPtr& origin, Budget& budget);
Json materialize(const NodePtr& node, Budget& budget);
std::vector<std::string> pointer_tokens(std::string_view pointer, const DefinitionLimits& limits);
std::string join_pointer(std::string_view left, std::string_view token, const DefinitionLimits& limits);
std::optional<RecordKey::Field> record_field(std::string_view path);
std::string config_key(const std::string& name);

struct Removal {
    DefinitionTarget target;
    OriginPtr origin;
};
struct LayerState {
    NodePtr root;
    std::vector<Removal> removals;
    std::vector<AuthorshipEvent> journal;
};

struct EditFailure {
    std::string code;
    std::string pointer;
    DefinitionTarget target;
};

DefinitionTarget target_from(const Json& value, std::string_view path_member = "path");
NodePtr target_node(const NodePtr& root, const DefinitionTarget& target, const DefinitionLimits& limits);
std::optional<SourceLocation> removed_at(const std::vector<Removal>& removals, const DefinitionTarget& target);
void apply_identity(LayerState& state, const Json& envelope, const DefinitionKey& key, Budget& budget);
using AuthoredValueObserver = std::function<void(const NodePtr&, const std::string&, DefinitionEditKind,
    const Json*, const SourceLocation&)>;
void apply_layer(LayerState& state, const Json& envelope, const DefinitionKey& key, Budget& budget,
    const AuthoredValueObserver& observer = {});
NodePtr effective_node(NodePtr node, std::string_view pointer, const DefinitionLimits& limits);
CompositionDiagnostics relationships(const Json& declaration, const LayerState& state, Budget& budget);
std::string digest(std::string_view domain, const Json& value, Budget& budget);
CompositionDiagnostics check_structure(const Json& value, DefinitionV2SchemaRoot root,
    const DefinitionLimits& limits, std::size_t& visits, Budget* budget = nullptr);
CompositionDiagnostics check_fragment(const Json& value, std::string_view fragment, Budget& budget);
void sort_diagnostics(CompositionDiagnostics& diagnostics);

struct ProvenanceData {
    NodePtr root;
    std::vector<Removal> removals;
    std::vector<AuthorshipEvent> journal;
};

}

namespace lemon::backends {

class DefinitionSourceSnapshot {
public:
    std::map<std::string, v2::SourceRecord> sources;
    std::size_t logical_bytes = 0;
};

struct ResolutionProvenance::Impl : v2::ProvenanceData {};

}
