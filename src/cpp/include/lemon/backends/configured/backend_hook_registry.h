#pragma once

#include "lemon/backends/backend_ops.h"
#include "lemon/backends/configured/backend_definition.h"

#include <functional>
#include <httplib.h>
#include <string_view>
#include <variant>

namespace lemon::backends {

struct LaunchHookContext {
    const json& values;
    const ModelInfo& model;
    const std::string& category;
};
struct OperationHookContext {
    const json& values;
    const std::string& category;
    int port;
    std::string base_url;
    std::function<json(const std::string&, const json&, long)> post_json;
};
enum class HookStage { Artifacts, Request, Response, Launch, Operation, StartFailure, SinkOperation };
using ArtifactHook = const BackendOps* (*)();
using RequestHook = json (*)(json request);
using ResponseHook = json (*)(json response, const json& request, const std::string& model_name);
using LaunchHook = json (*)(const LaunchHookContext& context);
using OperationHook = json (*)(const OperationHookContext& context, const json& request);
using StartFailureHook = std::string (*)(const std::string& executable, const std::vector<std::string>& args);
using SinkOperationHook = void (*)(const OperationHookContext& context, const json& request, httplib::DataSink& sink);
using HookTarget = std::variant<ArtifactHook, RequestHook, ResponseHook, LaunchHook, OperationHook, StartFailureHook, SinkOperationHook>;
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

const BackendHookRegistry& installed_backend_hooks();
void audit_hook_abi_metadata(const BackendHookRegistry& registry);

}
