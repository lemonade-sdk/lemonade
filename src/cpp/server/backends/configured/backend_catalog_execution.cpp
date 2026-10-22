#include "backend_catalog_admission_internal.h"
#include "backend_catalog_ledger_internal.h"
#include "lemon/backends/configured/backend_catalog.h"

#include <algorithm>
#include <type_traits>

namespace lemon::backends {
namespace {
DefinitionLimits bounded(DefinitionLimits value) {
    const DefinitionLimits hard;
#define LEMON_BOUND(field) value.field = std::min(value.field, hard.field)
    LEMON_BOUND(source_bytes); LEMON_BOUND(chain_documents); LEMON_BOUND(closure_source_bytes);
    LEMON_BOUND(source_depth); LEMON_BOUND(closure_source_nodes); LEMON_BOUND(edits_per_source);
    LEMON_BOUND(pointer_bytes); LEMON_BOUND(effective_depth); LEMON_BOUND(effective_bytes);
    LEMON_BOUND(effective_nodes); LEMON_BOUND(work_bytes); LEMON_BOUND(work_nodes);
    LEMON_BOUND(diagnostics); LEMON_BOUND(snapshot_documents); LEMON_BOUND(snapshot_source_bytes);
    LEMON_BOUND(retained_logical_bytes);
#undef LEMON_BOUND
    return value;
}

std::size_t allowance(const json& value) {
    accounting::work(0, 1);
    std::size_t bytes = sizeof(json) + 128;
    if (value.is_string()) bytes = v2::checked_sum(bytes, v2::checked_product(value.get_ref<const std::string&>().size(), 8));
    if (value.is_object()) for (const auto& [key, child] : value.items()) {
        accounting::work(key.size(), 0);
        bytes = v2::checked_sum(bytes, v2::checked_sum(v2::checked_product(key.size(), 8), allowance(child)));
    }
    else if (value.is_array()) for (const auto& child : value) bytes = v2::checked_sum(bytes, allowance(child));
    return bytes;
}
}

struct CatalogExecutionBuilder {
    static CatalogBuildResult build(std::shared_ptr<const FrozenBuiltinCatalog> builtins,
        const std::vector<ExternalDefinitionInput>& sources, const CatalogExecutionLimits& limits) {
        CatalogBuildResult result;
        auto ledger = std::make_shared<accounting::CatalogLedger>(limits);
        const auto fail = [&](const std::string& code, const std::string& id = "", const std::string& pointer = "") {
            result.catalog.reset();
            if (result.diagnostics.entries.size() < ledger->limits().diagnostics)
                result.diagnostics.entries.push_back({code, id, pointer, "Catalog execution finalization failed", {}, {}, {}});
            else result.diagnostics.truncated = true;
        };
        try {
            std::shared_ptr<const AdmittedCatalog> admitted;
            {
                accounting::StageScope stage(*ledger, CatalogBuildStage::Admission);
                if (!builtins) { fail("invalid_snapshot"); return result; }
                auto admission_limits = bounded(limits.admission);
                admission_limits.diagnostics = std::min(admission_limits.diagnostics, ledger->limits().diagnostics);
                if (builtins->definitions_ > admission_limits.snapshot_documents ||
                    sources.size() > admission_limits.snapshot_documents - builtins->definitions_) throw v2::LimitExceeded();
                std::size_t bytes = builtins->source_bytes_;
                v2::charge(bytes, 0, admission_limits.snapshot_source_bytes);
                for (const auto& input : sources) {
                    if (input.bytes.size() > admission_limits.source_bytes) throw v2::LimitExceeded();
                    v2::charge(bytes, input.bytes.size(), admission_limits.snapshot_source_bytes);
                }
                ledger->owned(sizeof(accounting::CatalogLedger) + sizeof(BackendCatalog) + builtins->logical_bytes_ + 256);
                ledger->receipt(builtins.get());
                ledger->dependency(builtins.get());
                const auto snapshot = make_catalog_source_snapshot(builtins, sources, admission_limits);
                const auto admission = snapshot.snapshot ? admit_catalog(snapshot.snapshot) : CatalogAdmissionResult{nullptr, snapshot.diagnostics};
                result.stages.resolved = admission.resolved;
                result.stages.authorized = admission.authorized;
                result.diagnostics = admission.diagnostics;
                if (!admission.catalog) { result.metrics = ledger->metrics(); return result; }
                admitted = admission.catalog;
                result.stages.resolved = result.stages.authorized = result.stages.catalog_admitted = true;
                ledger->receipt(admitted.get());
            }
            std::vector<std::shared_ptr<const AuthorizedDefinition>> tokens;
            std::vector<std::shared_ptr<const CompiledDefinition>> compiled;
            std::vector<std::shared_ptr<const ExecutableDefinition>> executables;
            std::size_t expected_entries = sources.size();
            {
                accounting::StageScope stage(*ledger, CatalogBuildStage::Compilation);
                accounting::Temporary inventory(v2::checked_sum(builtins->source_bytes_,
                    v2::checked_product(builtins->definitions_, sizeof(DefinitionKey) + 128)));
                const auto count = v2::checked_sum(builtins->definitions_, sources.size());
                accounting::owned(v2::checked_product(count, 4 * sizeof(std::shared_ptr<const void>) + 128));
                const auto builtin_keys = BuiltinCatalogLoader::builtin_leaves(*admitted->sources().authority_);
                expected_entries = v2::checked_sum(expected_entries, builtin_keys.size());
                for (const auto& key : builtin_keys) tokens.push_back(admitted->builtin_v2_definition(key));
                for (const auto& source : admitted->sources().sources_) if (source.header.version == 2) tokens.push_back(admitted->v2_definition(source.header.id));
                for (const auto& token : tokens) {
                    ledger->receipt(&token->definition());
                    auto compile_limits = limits.compilation;
                    compile_limits.diagnostics = std::min(compile_limits.diagnostics, ledger->limits().diagnostics);
                    auto result_compiled = compile_execution_definition(token, compile_limits);
                    if (!result_compiled.definition) { result.diagnostics = std::move(result_compiled.diagnostics); result.metrics = ledger->metrics(); return result; }
                    compiled.push_back(std::move(result_compiled.definition));
                }
                result.stages.compiled = true;
            }
            {
                accounting::StageScope stage(*ledger, CatalogBuildStage::Binding);
                for (const auto& program : compiled) {
                    auto bind_limits = limits.binding;
                    bind_limits.diagnostics = std::min(bind_limits.diagnostics, ledger->limits().diagnostics);
                    auto bound = bind_execution_definition(admitted, program, bind_limits);
                    if (!bound.definition) { result.diagnostics = std::move(bound.diagnostics); result.metrics = ledger->metrics(); return result; }
                    executables.push_back(std::move(bound.definition));
                }
                result.stages.bound = true;
            }
            std::shared_ptr<BackendCatalog> candidate;
            {
                accounting::StageScope stage(*ledger, CatalogBuildStage::Entries);
                accounting::owned(sizeof(BackendCatalogData) + 4096);
                candidate = std::shared_ptr<BackendCatalog>(new BackendCatalog(BackendCatalog::Unpublished{}, nullptr));
                candidate->builtins_ = std::move(builtins);
                candidate->admitted_ = admitted;
                const auto finalize = [&](const auto& definition) {
                    const auto& records = [&]() -> const json& {
                        if constexpr (std::is_same_v<typename std::decay_t<decltype(definition)>::element_type, const BackendDefinition>) return definition->records;
                        else return definition->compiled().normalized_declaration();
                    }();
                    const auto bytes = allowance(records);
                    accounting::owned(v2::checked_sum(v2::checked_product(bytes, 4), sizeof(ConfiguredBackendEntry) + 4096));
                    accounting::work(bytes, 1);
                    accounting::Temporary temporary(v2::checked_sum(v2::checked_product(bytes, 2), 8192));
                    candidate->entries_.push_back(ConfiguredBackendEntry::make(definition));
                };
                for (const auto& executable : executables) finalize(executable);
                for (const auto& source : admitted->sources().sources_) if (source.v1) finalize(source.v1);
                const auto builtins_allowance = candidate->builtins_->logical_bytes_;
                accounting::owned(v2::checked_sum(v2::checked_product(builtins_allowance, 2), 65536));
                accounting::work(builtins_allowance, 1);
                {
                    accounting::Temporary temporary(v2::checked_sum(v2::checked_product(ledger->metrics().stages[3].owned_bytes, 2), 8192));
                    std::stable_sort(candidate->entries_.begin(), candidate->entries_.end(), [&](const auto& left, const auto& right) {
                        const auto position = [&](const auto& entry) {
                            const auto& id = entry->definition ? entry->definition->id : entry->executable_definition->compiled().authorized().definition().key.id;
                            const auto found = admitted->sources().ids_.find(id);
                            return found == admitted->sources().ids_.end() ? std::size_t(0) : found->second + 1;
                        };
                        return position(left) < position(right);
                    });
                    candidate->finalize();
                }
                ledger->seal(expected_entries, candidate->entries_.size());
                candidate->ledger_ = ledger;
            }
            result.stages.executable = true;
            result.catalog = std::move(candidate);
        } catch (const DefinitionValidationError& error) {
            for (const auto& row : error.diagnostics) fail(row.code, row.definition, row.pointer);
        } catch (const v2::LimitExceeded&) { fail("limit_exceeded"); }
        catch (const std::exception&) { fail("invalid_binding"); }
        result.metrics = ledger->metrics();
        return result;
    }
};

CatalogBuildResult build_backend_catalog(std::shared_ptr<const FrozenBuiltinCatalog> builtins,
    const std::vector<ExternalDefinitionInput>& sources, const CatalogExecutionLimits& limits) {
    return CatalogExecutionBuilder::build(std::move(builtins), sources, limits);
}
const CatalogExecutionMetrics& BackendCatalog::execution_metrics() const noexcept { return ledger_->metrics(); }

}
