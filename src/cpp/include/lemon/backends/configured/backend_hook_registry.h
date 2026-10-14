#pragma once

#include "lemon/backends/backend_ops.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backends/configured/backend_definition.h"
#include "lemon/backends/configured/request_mapper.h"

#include <httplib.h>
#include <string_view>
#include <variant>

namespace lemon::backends {

struct InstallContext;
struct LaunchPlan;
struct ResolvedLaunchInputs;
class BackendTransport;
struct OperationContext {
    const BackendDefinition& definition;
    const OperationProgram& program;
    const MappingContext& mapping;
    const BackendTransport& transport;
};
enum class HookStage { Install, Artifacts, Launch, JsonOperation, SinkOperation };
using InstallHook = InstallParams (*)(const BackendDefinition&, const InstallContext&, const json&);
using ArtifactHook = std::string (*)(const BackendDefinition&, const ModelInfo&, const CheckpointResolveContext&, const json&);
using LaunchHook = LaunchPlan (*)(const BackendDefinition&, const ResolvedLaunchInputs&, const json&);
using JsonOperationHook = json (*)(const OperationContext&, const json&, const json&);
using SinkOperationHook = void (*)(const OperationContext&, const json&, httplib::DataSink&, const json&);
using HookTarget = std::variant<InstallHook, ArtifactHook, LaunchHook, JsonOperationHook, SinkOperationHook>;
struct HookRegistration {
    std::string id;
    HookStage stage;
    json parameter_schema;
    std::vector<OperationDescription> operations;
    HookTarget target;
};
class BackendHookRegistry {
public:
    explicit BackendHookRegistry(std::vector<HookRegistration> registrations);
    const HookRegistration& require(std::string_view id, HookStage expected_stage) const;
    const std::vector<HookRegistration>& registrations() const noexcept { return registrations_; }
private:
    const std::vector<HookRegistration> registrations_;
};

}
