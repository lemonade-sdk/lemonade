#pragma once

#include "lemon/backends/configured/backend_definition_composition.h"

namespace lemon::backends {

class BuiltinCatalogLoader;
class DefinitionAuthorizer;

class FrozenDefinitionSnapshot {
private:
    FrozenDefinitionSnapshot() = default;
    struct Impl;
    std::shared_ptr<const Impl> impl_;
    friend class BuiltinCatalogLoader;
    friend class DefinitionAuthorizer;
};

struct FrozenDefinitionSnapshotResult {
    std::shared_ptr<const FrozenDefinitionSnapshot> snapshot;
    CompositionDiagnostics diagnostics;
};

FrozenDefinitionSnapshotResult load_v2_authority_snapshot(
    const std::vector<DefinitionSourceView>& external_sources,
    const DefinitionLimits& limits = {});

class ResolvedAuthority {
private:
    ResolvedAuthority() = default;
    friend class AuthorizedDefinition;
    struct Impl;
    std::shared_ptr<const Impl> impl_;
    friend class DefinitionAuthorizer;
};

class AuthorizedDefinition {
public:
    const ResolvedDefinition& definition() const noexcept;
    const ResolvedAuthority& authority() const noexcept;

private:
    AuthorizedDefinition() = default;
    std::shared_ptr<const ResolvedDefinition> definition_;
    ResolvedAuthority authority_;
    friend class DefinitionAuthorizer;
};

struct DefinitionAuthorizationResult {
    std::shared_ptr<const AuthorizedDefinition> definition;
    CompositionDiagnostics diagnostics;
};

class DefinitionAuthorizer {
public:
    explicit DefinitionAuthorizer(std::shared_ptr<const FrozenDefinitionSnapshot> snapshot);
    DefinitionAuthorizationResult authorize(const DefinitionKey& leaf) const;

private:
    std::shared_ptr<const FrozenDefinitionSnapshot> snapshot_;
};

}
