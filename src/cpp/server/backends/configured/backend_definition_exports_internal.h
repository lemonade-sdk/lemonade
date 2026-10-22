#pragma once

#include "backend_definition_v2_internal.h"
#include "lemon/backends/configured/backend_definition_authorization.h"
#include "lemon/backends/configured/backend_definition_exports.h"
#include "lemon/backends/configured/backend_hook_abi.h"

namespace lemon::backends::v2 {

struct AuthorizationFailure {
    std::string code;
    SourceLocation source;
};

struct HookSelection {
    LogicalHookSlot slot;
    std::string pointer;
    std::string owner_pointer;
    std::string operation;
    Json parameters;
    Json owner_semantics;
    std::vector<std::string> ordered_hooks;
};

std::vector<HookSelection> hook_selections(const Json& declaration, Budget& budget);
bool accepts_mutable_value(const MutableDataRule& rule, const Json* value, Budget& budget);
Json slot_json(const LogicalHookSlot& slot);
Json slot_policy_json(const ExportSlotPolicy& policy);
const std::vector<std::string>& operation_owner_fields();
Json operation_owner_semantics(const Json& operation, Budget& budget);
std::vector<std::string> operation_hook_chain(const Json& operation, std::string_view stage, Budget& budget);
bool operation_owners_collide(const Json& left, const Json& right, Budget& budget);
Json owner_graph_json(const std::vector<OperationOwnerPolicy>& owners);
std::string owner_graph_digest(const std::vector<OperationOwnerPolicy>& owners);
Json export_policy_json(const BuiltinExportContract& contract);
Json hook_abi_json(const std::vector<HookAbiDescriptor>& descriptors);
std::string identity(std::string_view domain, const Json& value);
std::string parameter_digest(const Json& parameters);
std::string owner_semantics_digest(const Json& semantics);
std::string mutation_policy_digest(const ExportSlotPolicy& policy);
std::string export_policy_digest(const BuiltinExportContract& contract);
std::string export_registry_digest(const std::vector<BuiltinExportContract>& contracts,
    const std::vector<HookAbiDescriptor>& abis);
CompositionDiagnostics audit_export_contracts(const std::vector<BuiltinExportContract>& contracts,
    std::shared_ptr<const DefinitionSourceSnapshot> sources,
    const std::vector<HookAbiDescriptor>& abis,
    std::string_view expected_registry_digest,
    const DefinitionLimits& limits = {});

void verify_slot_shape(const HookSelection& selection, const ExportSlotPolicy& policy,
    const SourceLocation& source, Budget& budget);
std::vector<LogicalHookSlot> verify_surviving_grants(const ResolvedDefinition& definition,
    const ResolvedDefinition& parent, const ResolvedDefinition& base, const BuiltinExportContract* contract,
    const std::vector<LogicalHookSlot>& inherited_slots, const NodePtr& parent_root, const NodePtr& current_root,
    Budget& budget);

}

namespace lemon::backends {

struct BuiltinExecutionPolicies {
    std::vector<BuiltinExportContract> leaves;
    std::string digest;
};

const std::vector<DefinitionSourceView>& embedded_v2_sources();
BuiltinExportContract llamacpp_external_contract();
const BuiltinExecutionPolicies& fixed_builtin_v2_policies();
const BuiltinExecutionPolicies& embedded_v2_policies();

class BuiltinCatalogLoader {
public:
    static const BuiltinExportRegistry& installed_exports();
    static const BuiltinExportRegistry& selected_exports();
    static FrozenDefinitionSnapshotResult freeze(
        const std::vector<DefinitionSourceView>& external_sources, const DefinitionLimits& limits);
    static FrozenDefinitionSnapshotResult compose(std::shared_ptr<const FrozenDefinitionSnapshot> builtins,
        const std::vector<DefinitionSourceView>& external_sources, const DefinitionLimits& limits, std::size_t& retained);
    static FrozenDefinitionSnapshotResult freeze_embedded(const DefinitionLimits& limits = {});
    static std::vector<DefinitionKey> builtin_leaves(const FrozenDefinitionSnapshot& snapshot);
#ifdef LEMON_BACKEND_EXPORT_TESTING
    static FrozenDefinitionSnapshotResult test_freeze(
        const std::vector<DefinitionSourceView>& sources,
        std::shared_ptr<const BuiltinExportRegistry> registry, const DefinitionLimits& limits = {});
    static FrozenDefinitionSnapshotResult test_freeze_builtins(
        const std::vector<DefinitionSourceView>& sources, const BuiltinExecutionPolicies& policies,
        const DefinitionLimits& limits = {});
#endif

private:
    static FrozenDefinitionSnapshotResult freeze_sources(
        const std::vector<DefinitionSourceView>& sources,
        std::shared_ptr<const BuiltinExportRegistry> registry, const DefinitionLimits& limits, std::size_t* retained = nullptr,
        std::shared_ptr<const BuiltinExecutionPolicies> builtin_policies = {});
    static DefinitionAuthorizationResult authorize_builtin(std::shared_ptr<const FrozenDefinitionSnapshot> snapshot,
        const DefinitionKey& leaf, std::size_t* retained);
    friend struct CatalogAdmissionBuilder;

public:
#ifdef LEMON_BACKEND_EXPORT_TESTING
    static std::shared_ptr<const BuiltinExportRegistry> test_exports(
        const std::vector<BuiltinExportContract>& contracts,
        std::shared_ptr<const DefinitionSourceSnapshot> sources,
        const std::vector<HookAbiDescriptor>& abis, std::string_view expected_digest);
#endif
};

}
