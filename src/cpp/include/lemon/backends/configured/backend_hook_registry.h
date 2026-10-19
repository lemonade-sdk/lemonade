#pragma once

#include "lemon/backends/backend_ops.h"
#include "lemon/backends/configured/backend_definition.h"

#include <string_view>
#include <variant>

namespace lemon::backends {

enum class HookStage { Artifacts, Request, Response };
using ArtifactHook = const BackendOps* (*)();
using RequestHook = json (*)(json request);
using ResponseHook = json (*)(json response, const json& request, const std::string& model_name);
using HookTarget = std::variant<ArtifactHook, RequestHook, ResponseHook>;
struct HookRegistration {
    std::string id;
    HookStage stage;
    HookTarget target;
};
class BackendHookRegistry {
public:
    explicit BackendHookRegistry(std::vector<HookRegistration> registrations);
    const HookRegistration& require(std::string_view id, HookStage expected_stage) const;
    template<class Target>
    Target target(std::string_view id, HookStage expected_stage) const { return std::get<Target>(require(id, expected_stage).target); }
    const std::vector<HookRegistration>& registrations() const noexcept { return registrations_; }
private:
    const std::vector<HookRegistration> registrations_;
};

std::string_view hook_stage_name(HookStage stage);
const BackendHookRegistry& installed_backend_hooks();

}
