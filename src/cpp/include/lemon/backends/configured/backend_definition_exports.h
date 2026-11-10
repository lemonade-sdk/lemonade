#pragma once

#include "lemon/backends/configured/backend_definition_composition.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lemon::backends {

struct BuiltinExportKey {
    std::string id;
    std::uint32_t contract;
};

enum class SlotOwnerKind { Root, Operation, Artifact };

struct LogicalHookSlot {
    SlotOwnerKind owner_kind;
    std::string owner_id;
    std::string stage;
    std::string hook_id;
};

enum class SlotRemoval { Forbidden, WithOwner, Independent };
enum class MutableValueType { Boolean, Integer, Number, String, Expression };
enum class HookEffect { Pure, ManagedInstallRead, ManagedInstallWrite, ProcessProbe };

struct MutableDataRule {
    DefinitionTarget target;
    MutableValueType type;
    bool allow_absent = false;
    std::optional<double> minimum;
    std::optional<double> maximum;
    std::size_t maximum_length = 0;
    nlohmann::json values = nlohmann::json::array();
    std::string schema_fragment;
    std::vector<std::string> reference_roots;
};

enum class MutableFragmentKind {
    OptionPresentationV1, OptionDefaultV1, ArgvSpellingsV1, ReservedFlagsV1,
    CustomDefaultsV1, DeviceSelectionV1, EnvironmentEntriesV1, SupportRowsV1,
    InstallCasesV1, ContextFloorsV1, LocalExecutableV1
};

enum class ReferencePhase { Input, RuntimeOptions, InstallRepository, InstallAsset, Artifacts, Prepare, Launch, Request };

struct TypedDataReference {
    std::string path;
    MutableValueType type;
    ReferencePhase phase;
};

struct CollectionPermissions {
    bool add = false;
    bool remove = false;
    bool clear = false;
};

struct MutableFragmentRule {
    MutableFragmentKind kind;
    DefinitionTarget target;
    std::vector<std::string> fields;
    CollectionPermissions collection;
    std::size_t maximum_records = 64;
    std::size_t maximum_length = 4096;
    double minimum = 0;
    double maximum = 1048576;
    std::vector<std::pair<std::string, std::vector<std::string>>> domains;
    std::vector<std::string> required_records;
    std::vector<TypedDataReference> references;
};

struct ExportSlotPolicy {
    LogicalHookSlot slot;
    std::uint32_t hook_abi_revision;
    std::string parameter_digest;
    std::string mutation_policy_digest;
    SlotRemoval removal;
    std::vector<LogicalHookSlot> required_slots;
    std::vector<DefinitionTarget> sealed_inputs;
    std::vector<MutableDataRule> mutable_inputs;
    std::vector<std::string> required_phases;
    std::vector<HookEffect> effects;
    std::string owner_semantics_digest;
    std::vector<std::string> ordered_hooks;
    std::vector<MutableFragmentRule> mutable_fragments;
};

struct OperationOwnerPolicy {
    std::string id;
    nlohmann::json semantics;
    std::vector<std::string> request_hooks;
    std::vector<std::string> response_hooks;
    std::vector<std::string> handler_hooks;
    bool allow_removal = false;
    std::vector<std::string> required_owners;
    std::vector<std::string> sealed_fields;
    std::vector<std::string> collisions;
};

struct BuiltinExportContract {
    BuiltinExportKey key;
    std::string expected_source_digest;
    std::string expected_definition_hash;
    std::string expected_dependency_digest;
    std::uint32_t policy_revision;
    std::string policy_digest;
    std::vector<ExportSlotPolicy> slots;
    std::vector<OperationOwnerPolicy> owners;
    std::string owner_graph_digest;
    std::vector<MutableFragmentRule> mutable_fragments;
};

class BuiltinCatalogLoader;

class BuiltinExportRegistry {
public:
    const BuiltinExportContract* find(std::string_view id, std::uint32_t contract) const noexcept;
    const std::string& digest() const noexcept;
    const std::vector<BuiltinExportContract>& exports() const noexcept;

private:
    BuiltinExportRegistry() = default;
    struct Impl;
    std::shared_ptr<const Impl> impl_;
    friend class BuiltinCatalogLoader;
};

const BuiltinExportRegistry& installed_builtin_exports();

}
