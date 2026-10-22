#include "backend_catalog_ledger_internal.h"
#include "backend_definition_v2_internal.h"

#include <algorithm>
#include <set>

namespace lemon::backends {
namespace {
using namespace v2;

DefinitionKey parent_key(const Json& parent) {
    return {parent.contains("builtin") ? DefinitionNamespace::Builtin : DefinitionNamespace::External,
        parent.at(parent.contains("builtin") ? "builtin" : "external").get<std::string>(), parent.value("contract", std::uint32_t(0))};
}

Json key_json(const DefinitionKey& key) {
    return {{"namespace", key.space == DefinitionNamespace::Builtin ? "builtin" : "external"}, {"id", key.id}, {"contract", key.contract}};
}


void remap(CompositionDiagnostics& diagnostics, const LayerState& state, Budget& budget) {
    const auto& limits = budget.limits;
    for (auto& diagnostic : diagnostics.entries) {
        budget.keep(v2::checked_sum(sizeof(SourceLocation), v2::checked_product(limits.pointer_bytes, 4)));
        budget.work(diagnostic.pointer.size(), 1);
        const auto effective = diagnostic.pointer;
        const auto origin_node = effective_node(state.root, effective, limits)->origin;
        budget.keep(v2::checked_product(origin_node->document->id.size(), 2));
        auto origin = location(origin_node);
        if (diagnostic.code == "required") if (const auto removed = removed_at(state.removals, {effective, {}, ""})) {
            const auto rank = [&](const SourceLocation& source) {
                std::size_t result = 0;
                for (std::size_t i = 0; i < state.journal.size(); ++i) {
                    const auto& event = state.journal[i].source;
                    budget.work(v2::checked_sum(event.document.id.size(), source.pointer.size()), 1);
                    if (event.document.space == source.document.space && event.document.contract == source.document.contract && event.document.id == source.document.id && (source.pointer == event.pointer || source.pointer.rfind(event.pointer + "/", 0) == 0)) result = i + 1;
                }
                return result;
            };
            if (rank(*removed) > rank(origin)) origin = *removed;
        }
        diagnostic.effective_pointer = effective;
        diagnostic.origin = origin;
        diagnostic.definition = origin.document.id;
        diagnostic.pointer = origin.pointer;
    }
    sort_diagnostics(diagnostics);
}

}

struct DefinitionResolver::Impl {
    struct Cached {
        std::shared_ptr<const ResolvedDefinition> definition;
        std::shared_ptr<const ResolutionProvenance::Impl> state;
    };
    std::shared_ptr<const DefinitionSourceSnapshot> snapshot;
    DefinitionLimits limits;
    std::size_t owned_retained = 0;
    std::size_t* shared_retained = nullptr;
    std::map<std::string, Cached> cache;

    DefinitionResolutionResult run(const DefinitionKey& leaf) {
        auto& retained = shared_retained ? *shared_retained : owned_retained;
        DefinitionResolutionResult result;
        const auto error = [&](const DefinitionKey& key, const std::string& code, const std::string& pointer, const std::vector<SourceLocation>& related = std::vector<SourceLocation>{}) {
            if (limits.diagnostics == 0) { result.diagnostics.truncated = true; return; }
            result.diagnostics.entries.push_back({code, key.id, pointer, "Definition resolution failed", {}, SourceLocation{key, pointer}, {}});
            auto& diagnostic = result.diagnostics.entries.back();
            for (const auto& origin : related) {
                if (diagnostic.related.size() + result.diagnostics.entries.size() >= limits.diagnostics) { result.diagnostics.truncated = true; break; }
                diagnostic.related.push_back(origin);
            }
        };
        DefinitionKey current = leaf;
        std::vector<const v2::SourceRecord*> chain;
        std::set<std::string> active;
        std::size_t bytes = 0, nodes = 0;
        try {
            if (!snapshot) { error(leaf, "invalid_snapshot", ""); return result; }
            while (true) {
                const auto name = v2::key_string(current);
                if (active.count(name)) {
                    std::vector<SourceLocation> cycle;
                    auto begin = std::find_if(chain.begin(), chain.end(), [&](const auto* record) { return v2::key_string(record->key) == name; });
                    for (auto it = begin; it != chain.end(); ++it) cycle.push_back({(*it)->key, "/extends"});
                    cycle.push_back({current, "/extends"});
                    error(chain.back()->key, "dependency_cycle", "/extends", cycle);
                    return result;
                }
                const auto found = snapshot->sources.find(name);
                if (found == snapshot->sources.end()) {
                    const auto code = std::any_of(snapshot->sources.begin(), snapshot->sources.end(), [&](const auto& pair) { return pair.second.key.space == current.space && pair.second.key.id == current.id; }) ? "contract_mismatch" : "missing_parent";
                    error(chain.empty() ? current : chain.back()->key, code, chain.empty() ? "" : "/extends");
                    return result;
                }
                if (chain.size() >= limits.chain_documents) throw v2::LimitExceeded();
                v2::charge(bytes, found->second.bytes.size(), limits.closure_source_bytes);
                v2::charge(nodes, found->second.nodes, limits.closure_source_nodes);
                const auto admission = v2::preflight_source(found->second, limits);
                if (!admission.entries.empty() || admission.truncated) { result.diagnostics = admission; return result; }
                active.insert(name);
                chain.push_back(&found->second);
                if (!found->second.header.contains("extends")) break;
                current = parent_key(found->second.header["extends"]);
            }
            std::reverse(chain.begin(), chain.end());
            const Cached* parent = nullptr;
            for (std::size_t index = 0; index < chain.size(); ++index) {
                const auto& source = *chain[index];
                current = source.key;
                const auto cached = cache.find(v2::key_string(current));
                if (cached != cache.end()) {
                    if (cached->second.definition->metrics.work_bytes > limits.work_bytes || cached->second.definition->metrics.work_nodes > limits.work_nodes) throw v2::LimitExceeded();
                    accounting::work(cached->second.definition->metrics.effective_bytes, 1);
                    v2::charge(retained, cached->second.definition->metrics.effective_bytes, limits.retained_logical_bytes);
                    parent = &cached->second;
                    continue;
                }
                v2::Budget budget{limits, parent ? parent->definition->metrics : ResolutionMetrics{}, retained};
                v2::charge(budget.metrics.closure_source_bytes, source.bytes.size(), limits.closure_source_bytes);
                v2::charge(budget.metrics.closure_source_nodes, source.nodes, limits.closure_source_nodes);
                budget.work(source.bytes.size(), source.nodes);
                budget.keep(v2::checked_sum(source.bytes.size(), v2::checked_product(source.nodes, sizeof(v2::Json))));
                ++v2::admission_counters.dom;
                const auto envelope = v2::Json::parse(source.bytes);
                std::size_t visits = 0;
                auto structural = v2::check_structure(envelope, DefinitionV2SchemaRoot::Envelope, limits, visits, &budget);
                budget.work(0, visits);
                if (!structural.entries.empty() || structural.truncated) {
                    for (auto& diagnostic : structural.entries) { diagnostic.definition = source.key.id; diagnostic.origin = SourceLocation{source.key, diagnostic.pointer}; }
                    result.diagnostics = std::move(structural);
                    return result;
                }
                if (parent && envelope["extends"].contains("expected_digest") && envelope["extends"]["expected_digest"] != parent->definition->definition_hash) {
                    error(source.key, "expected_digest_mismatch", "/extends/expected_digest", {{parent->definition->key, ""}});
                    return result;
                }
                v2::LayerState state;
                if (parent) {
                    budget.admit(parent->state->root->metrics);
                    budget.work(parent->state->root->metrics.bytes, parent->state->root->metrics.nodes);
                    accounting::owned(sizeof(v2::LayerState));
                    v2::charge(retained, v2::checked_sum(parent->state->root->metrics.bytes, sizeof(v2::LayerState)), limits.retained_logical_bytes);
                    budget.metrics.retained_logical_bytes = retained;
                    for (const auto& event : parent->state->journal) {
                        const auto strings = v2::checked_sum(event.source.document.id.size(), v2::checked_sum(event.source.pointer.size(), v2::checked_sum(event.target.path.size(), v2::checked_sum(event.target.at.size(), event.target.match ? event.target.match->value.size() : 0))));
                        budget.keep(v2::checked_sum(sizeof(AuthorshipEvent), strings));
                        budget.work(strings, 1);
                    }
                    for (const auto& removal : parent->state->removals) budget.keep(v2::checked_sum(sizeof(v2::Removal), v2::checked_sum(removal.target.path.size(), v2::checked_sum(removal.target.at.size(), removal.target.match ? removal.target.match->value.size() : 0))));
                    state.root = parent->state->root;
                    state.removals = parent->state->removals;
                    state.journal = parent->state->journal;
                }
                try { v2::apply_layer(state, envelope, source.key, budget); }
                catch (const v2::EditFailure& failure) {
                    std::vector<SourceLocation> related;
                    if (const auto removed = v2::removed_at(state.removals, failure.target)) related.push_back(*removed);
                    error(source.key, failure.code, failure.pointer, related);
                    return result;
                }
                v2::apply_identity(state, envelope, source.key, budget);
                auto declaration = v2::materialize(state.root, budget);
                structural = v2::check_structure(declaration, DefinitionV2SchemaRoot::Declaration, limits, visits, &budget);
                budget.work(0, visits);
                if (structural.entries.empty() && !structural.truncated) structural = v2::relationships(declaration, state, budget);
                if (!structural.entries.empty() || structural.truncated) {
                    remap(structural, state, budget);
                    result.diagnostics = std::move(structural);
                    return result;
                }
                budget.keep(sizeof(ResolvedDefinition) + sizeof(ResolutionProvenance::Impl) + sizeof(ResolutionProvenance));
                auto resolved = std::make_shared<ResolvedDefinition>();
                resolved->key = source.key;
                resolved->declaration = std::move(declaration);
                resolved->source_digest = v2::digest("lemonade.backend-source.v2\n", envelope, budget);
                resolved->definition_hash = v2::digest("lemonade.backend-definition.v2\n", resolved->declaration, budget);
                auto parents = v2::Json::array();
                if (parent) {
                    for (const auto& dependency : parent->definition->dependencies) budget.keep(v2::checked_sum(sizeof(DependencyIdentity) + 142, dependency.document.id.size()));
                    budget.keep(v2::checked_sum(sizeof(DependencyIdentity) + 142, parent->definition->key.id.size()));
                    resolved->dependencies = parent->definition->dependencies;
                    resolved->dependencies.push_back({parent->definition->key, parent->definition->source_digest, parent->definition->definition_hash});
                }
                for (const auto& dependency : resolved->dependencies) {
                    budget.keep(v2::checked_sum(512, dependency.document.id.size()));
                    parents.push_back({{"document", key_json(dependency.document)}, {"source_digest", dependency.source_digest}, {"definition_hash", dependency.definition_hash}});
                }
                resolved->dependency_digest = v2::digest("lemonade.backend-dependencies.v2\n", {{"resolver_revision", 1}, {"leaf_source_digest", resolved->source_digest}, {"parents", parents}}, budget);
                auto provenance = std::make_shared<ResolutionProvenance>();
                auto data = std::make_shared<ResolutionProvenance::Impl>();
                data->root = state.root;
                data->removals = std::move(state.removals);
                data->journal = std::move(state.journal);
                provenance->impl_ = data;
                budget.keep(v2::checked_sum(sizeof(Cached) + 213, v2::checked_sum(source.key.id.size(), v2::key_string(source.key).size())));
                resolved->provenance = std::move(provenance);
                resolved->metrics = budget.metrics;
                resolved->metrics.effective_bytes = state.root->metrics.bytes;
                resolved->metrics.effective_nodes = state.root->metrics.nodes;
                const auto inserted = cache.emplace(v2::key_string(source.key), Cached{resolved, std::move(data)});
                parent = &inserted.first->second;
            }
            result.definition = parent->definition;
        } catch (const v2::LimitExceeded&) { error(current, "limit_exceeded", chain.empty() ? "" : "/extends"); }
        return result;
    }
};

DefinitionResolver::DefinitionResolver(std::shared_ptr<const DefinitionSourceSnapshot> snapshot, DefinitionLimits limits)
    : DefinitionResolver(std::move(snapshot), limits, nullptr) {}

DefinitionResolver::DefinitionResolver(std::shared_ptr<const DefinitionSourceSnapshot> snapshot,
    DefinitionLimits limits, std::size_t* retained) : impl_(std::make_unique<Impl>()) {
    impl_->snapshot = std::move(snapshot);
    impl_->limits = limits;
    impl_->owned_retained = impl_->snapshot ? impl_->snapshot->logical_bytes : 0;
    impl_->shared_retained = retained;
}
DefinitionResolver::~DefinitionResolver() = default;
DefinitionResolver::DefinitionResolver(DefinitionResolver&&) noexcept = default;
DefinitionResolver& DefinitionResolver::operator=(DefinitionResolver&&) noexcept = default;
DefinitionResolutionResult DefinitionResolver::resolve(const DefinitionKey& leaf) { return impl_->run(leaf); }

}
