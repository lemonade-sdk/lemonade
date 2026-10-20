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

class BuiltinCatalogLoader {
public:
    static const BuiltinExportRegistry& installed_exports();
    static FrozenDefinitionSnapshotResult freeze(
        const std::vector<DefinitionSourceView>& external_sources, const DefinitionLimits& limits);
    static FrozenDefinitionSnapshotResult test_freeze(
        const std::vector<DefinitionSourceView>& sources,
        std::shared_ptr<const BuiltinExportRegistry> registry, const DefinitionLimits& limits = {});

private:
    static FrozenDefinitionSnapshotResult freeze_sources(
        const std::vector<DefinitionSourceView>& sources,
        std::shared_ptr<const BuiltinExportRegistry> registry, const DefinitionLimits& limits);

public:
    static std::shared_ptr<const BuiltinExportRegistry> test_exports(
        const std::vector<BuiltinExportContract>& contracts,
        std::shared_ptr<const DefinitionSourceSnapshot> sources,
        const std::vector<HookAbiDescriptor>& abis, std::string_view expected_digest);
};

}
