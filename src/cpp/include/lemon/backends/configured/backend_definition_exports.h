#pragma once

#include "lemon/backends/configured/backend_definition_composition.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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
