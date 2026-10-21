#pragma once

#include "backend_definition_binding_internal.h"
#include "backend_runtime_services_internal.h"
#include "lemon/backends/backend_ops.h"
#include "lemon/backends/configured/backend_transport.h"

namespace lemon::backends {

struct ExecutionSession {
    std::shared_ptr<const ExecutableDefinition> executable;
    execution::InvocationFrame frame;
    std::set<typed::Fact> facts;
    std::set<std::size_t> produced;
    std::string model_name;
};
struct PreparedExecution {
    LaunchPlan plan;
    std::shared_ptr<const ExecutionSession> session;
};

class DefinitionRunner final {
public:
    explicit DefinitionRunner(std::shared_ptr<const ExecutableDefinition> executable);
    const nlohmann::json& declaration() const;
    std::vector<OperationBinding> operation_bindings() const;
    PreparedExecution prepare_load(const LoadContext& context, const BackendSpec& spec,
        LaunchServices& services, utils::RequestCancelToken cancel = {}) const;
    TransportRequest map_request(BackendOperation operation, OperationDelivery delivery, ModelType model_type,
        const ExecutionSession& session, const json& request, std::optional<std::string_view> original_body = {},
        utils::RequestCancelToken cancel = {}) const;
    json map_response(BackendOperation operation, ModelType model_type, const ExecutionSession& session,
        const json& request, json response, utils::RequestCancelToken cancel = {}) const;
    void resolve_runtime_options(const ModelInfo& model, RecipeOptions& options) const;
private:
    const execution::OperationProgram& cell(BackendOperation operation, OperationDelivery delivery, ModelType model_type) const;
    runtime::Frame request_frame(const ExecutionSession& session, const json& request) const;
    std::vector<std::size_t> slots(SlotOwnerKind kind, const std::string& owner, const std::string& stage) const;
    std::optional<json> evaluate(const std::string& expression, const runtime::Frame& frame) const;
    void defaults(runtime::Frame& frame, const std::string& stage) const;
    template<class Target, class Call>
    auto invoke(runtime::Frame& frame, std::size_t index, SlotOwnerKind kind, const std::string& owner,
        const std::string& stage, utils::RequestCancelToken cancel, Call call) const {
        const auto& hook = program_.hooks.at(index);
        const auto& bound = binding::Inspection::hooks(*executable_).at(index);
        if (bound.symbolic_index != index) throw std::logic_error("Bound slot index mismatch");
        runtime::InvocationGuard guard(frame, *executable_, index, hook, kind, owner, stage, std::move(cancel));
        auto result = call(std::get<Target>(bound.target), guard);
        guard.check_active();
        frame.produced.insert(index);
        frame.facts.insert(hook.provides.begin(), hook.provides.end());
        return result;
    }
    const std::shared_ptr<const ExecutableDefinition> executable_;
    const execution::Program& program_;
};

}
