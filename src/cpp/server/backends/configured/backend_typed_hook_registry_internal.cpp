#include "backend_typed_hook_registry_internal.h"

#include <array>
#include <stdexcept>
#include <type_traits>

namespace lemon::backends::typed {
namespace {

Registration expected(Contract contract) {
    using C = Contract;
    using E = Effect;
    using F = Fact;
    switch (contract) {
        case C::Backend: return {C::Backend, "llamacpp.backend.v1", "backend",
            BackendSelectionOps{&llamacpp::select_backend, &llamacpp::validate_device}, 1, 2,
            {E::SupportQuery, E::DeviceQuery}, {F::BackendOptions}, {F::SelectedBackend}};
        case C::InstallContext: return {C::InstallContext, "llamacpp.install_context.v1", "install_context",
            InstallContextHook(&llamacpp::install_context), 2, 2,
            {E::InstallFactRead}, {F::SelectedBackend, F::Platform, F::VersionInputs}, {F::InstallFacts}};
        case C::RuntimeOptions: return {C::RuntimeOptions, "llamacpp.runtime_options.v1", "runtime_options",
            RuntimeOptionsHook(&llamacpp::runtime_options), 1, 1,
            {}, {F::ModelLabels, F::DraftCheckpoint}, {F::RuntimeFacts}};
        case C::Prepare: return {C::Prepare, "llamacpp.prepare.v1", "launch",
            PrepareHook(&llamacpp::prepare), 1, 1,
            {}, {F::ModelArtifacts, F::ModelLabels, F::DraftCheckpoint, F::ContextSize, F::SelectedBackend}, {F::ModelFacts}};
        case C::Environment: return {C::Environment, "llamacpp.environment.v1", "environment",
            EnvironmentHook(&llamacpp::environment), 3, 3,
            {E::ParentEnvironmentRead, E::RuntimeFactRead, E::StagingPlan},
            {F::Platform, F::SelectedBackend, F::ResolvedProgram}, {F::EnvironmentPlan}};
        case C::System: return {C::System, "llamacpp.system.v1", "system",
            SystemOps{&llamacpp::resolve_version, &llamacpp::check_install}, 1, 1,
            {E::SystemProbe}, {F::Platform, F::SelectedBackend, F::VersionInputs}, {F::SystemFacts}};
        case C::Artifacts: return {C::Artifacts, "llamacpp.artifacts.v1", "artifacts",
            ArtifactOps{&llamacpp::metadata, &llamacpp::validate_registration, &llamacpp::validate_file,
                &llamacpp::resolve_path, &llamacpp::find_imported}, 2, 2,
            {E::ArtifactRead}, {F::ModelArtifacts, F::ModelLabels}, {F::ArtifactMetadata}};
        case C::MaxTokensAlias: return {C::MaxTokensAlias, "llamacpp.max_tokens_alias", "request",
            RequestTransform(&llamacpp::max_tokens_alias), 1, 1, {}, {F::Request}, {F::Request}};
        case C::ToolSchemaLimits: return {C::ToolSchemaLimits, "llamacpp.tool_schema_limits", "request",
            RequestTransform(&llamacpp::tool_schema_limits), 1, 1, {}, {F::Request}, {F::Request}};
        case C::ClientModel: return {C::ClientModel, "llamacpp.client_model", "response",
            ResponseTransform(&llamacpp::client_model), 1, 1, {}, {F::Request, F::Response}, {F::Response}};
    }
    throw std::logic_error("ExactTenTypedTargets: unknown contract");
}

bool exact_target(const Target& actual, const Target& wanted) {
    if (actual.index() != wanted.index()) return false;
    return std::visit([&](const auto& a) {
        using T = std::decay_t<decltype(a)>;
        const auto& b = std::get<T>(wanted);
        if constexpr (std::is_same_v<T, BackendSelectionOps>) {
            return a.select_backend == b.select_backend && a.validate_device == b.validate_device;
        } else if constexpr (std::is_same_v<T, SystemOps>) {
            return a.resolve_version == b.resolve_version && a.check_install == b.check_install;
        } else if constexpr (std::is_same_v<T, ArtifactOps>) {
            return a.metadata == b.metadata && a.validate_registration == b.validate_registration &&
                a.validate_file == b.validate_file && a.resolve_path == b.resolve_path && a.find_imported == b.find_imported;
        } else {
            return a == b;
        }
    }, actual);
}

}

const std::vector<Registration>& installed_targets() {
    using C = Contract;
    using E = Effect;
    using F = Fact;
    static const std::vector<Registration> entries = {
        {C::Artifacts, "llamacpp.artifacts.v1", "artifacts",
            ArtifactOps{&llamacpp::metadata, &llamacpp::validate_registration, &llamacpp::validate_file,
                &llamacpp::resolve_path, &llamacpp::find_imported}, 2, 2,
            {E::ArtifactRead}, {F::ModelArtifacts, F::ModelLabels}, {F::ArtifactMetadata}},
        {C::Backend, "llamacpp.backend.v1", "backend",
            BackendSelectionOps{&llamacpp::select_backend, &llamacpp::validate_device}, 1, 2,
            {E::SupportQuery, E::DeviceQuery}, {F::BackendOptions}, {F::SelectedBackend}},
        {C::ClientModel, "llamacpp.client_model", "response", ResponseTransform(&llamacpp::client_model),
            1, 1, {}, {F::Request, F::Response}, {F::Response}},
        {C::Environment, "llamacpp.environment.v1", "environment", EnvironmentHook(&llamacpp::environment),
            3, 3, {E::ParentEnvironmentRead, E::RuntimeFactRead, E::StagingPlan},
            {F::Platform, F::SelectedBackend, F::ResolvedProgram}, {F::EnvironmentPlan}},
        {C::InstallContext, "llamacpp.install_context.v1", "install_context", InstallContextHook(&llamacpp::install_context),
            2, 2, {E::InstallFactRead}, {F::SelectedBackend, F::Platform, F::VersionInputs}, {F::InstallFacts}},
        {C::MaxTokensAlias, "llamacpp.max_tokens_alias", "request", RequestTransform(&llamacpp::max_tokens_alias),
            1, 1, {}, {F::Request}, {F::Request}},
        {C::Prepare, "llamacpp.prepare.v1", "launch", PrepareHook(&llamacpp::prepare),
            1, 1, {}, {F::ModelArtifacts, F::ModelLabels, F::DraftCheckpoint, F::ContextSize, F::SelectedBackend}, {F::ModelFacts}},
        {C::RuntimeOptions, "llamacpp.runtime_options.v1", "runtime_options", RuntimeOptionsHook(&llamacpp::runtime_options),
            1, 1, {}, {F::ModelLabels, F::DraftCheckpoint}, {F::RuntimeFacts}},
        {C::System, "llamacpp.system.v1", "system", SystemOps{&llamacpp::resolve_version, &llamacpp::check_install},
            1, 1, {E::SystemProbe}, {F::Platform, F::SelectedBackend, F::VersionInputs}, {F::SystemFacts}},
        {C::ToolSchemaLimits, "llamacpp.tool_schema_limits", "request", RequestTransform(&llamacpp::tool_schema_limits),
            1, 1, {}, {F::Request}, {F::Request}}
    };
    return entries;
}

void audit_targets(const std::vector<Registration>& entries) {
    if (entries.size() != 10) throw std::logic_error("ExactTenTypedTargets: inventory size");
    std::array<bool, 10> seen{};
    for (const auto& entry : entries) {
        const auto wanted = expected(entry.contract);
        const auto index = static_cast<std::size_t>(entry.contract);
        if (index >= seen.size() || seen[index] || entry.id != wanted.id || entry.stage != wanted.stage ||
            !exact_target(entry.target, wanted.target)) {
            throw std::logic_error("ExactTenTypedTargets: identity or callable mismatch");
        }
        seen[index] = true;
        if (entry.abi_revision != wanted.abi_revision || entry.implementation_revision != wanted.implementation_revision ||
            entry.effects != wanted.effects || entry.requires != wanted.requires || entry.provides != wanted.provides) {
            throw std::logic_error("TypedEffectsAndDependenciesPinned: metadata mismatch");
        }
    }
}

}
