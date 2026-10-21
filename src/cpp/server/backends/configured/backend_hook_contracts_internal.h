#pragma once

#include <cstdint>

namespace lemon::backends::typed {

enum class Contract {
    Backend, InstallContext, RuntimeOptions, Prepare, Environment, System, Artifacts,
    MaxTokensAlias, ToolSchemaLimits, ClientModel
};

enum class Effect {
    SupportQuery, DeviceQuery, InstallFactRead, ParentEnvironmentRead,
    RuntimeFactRead, StagingPlan, SystemProbe, ArtifactRead
};

enum class Fact {
    BackendOptions, Platform, VersionInputs, ModelLabels, DraftCheckpoint,
    ModelArtifacts, ContextSize, SelectedBackend, ResolvedProgram, Request, Response,
    BackendSelection, InstallFacts, RuntimeFacts, ModelFacts, EnvironmentPlan,
    SystemFacts, ArtifactMetadata
};

}
