#pragma once

#include "lemon/backends/configured/backend_definition_composition.h"

namespace lemon::backends {

enum class DefinitionV2SchemaRoot { Envelope, Declaration };

CompositionDiagnostics validate_v2_structure(
    const nlohmann::json& value,
    DefinitionV2SchemaRoot root,
    const DefinitionLimits& limits = {});

class DefinitionSourceSnapshot;

struct DefinitionSnapshotResult {
    std::shared_ptr<const DefinitionSourceSnapshot> snapshot;
    CompositionDiagnostics diagnostics;
};

DefinitionSnapshotResult make_v2_source_snapshot(
    const std::vector<DefinitionSourceView>& sources,
    const DefinitionLimits& limits = {});

class DefinitionResolver {
public:
    explicit DefinitionResolver(
        std::shared_ptr<const DefinitionSourceSnapshot> snapshot,
        DefinitionLimits limits = {});
    ~DefinitionResolver();

    DefinitionResolver(DefinitionResolver&&) noexcept;
    DefinitionResolver& operator=(DefinitionResolver&&) noexcept;
    DefinitionResolver(const DefinitionResolver&) = delete;
    DefinitionResolver& operator=(const DefinitionResolver&) = delete;

    DefinitionResolutionResult resolve(const DefinitionKey& leaf);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
