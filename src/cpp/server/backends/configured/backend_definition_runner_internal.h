#pragma once

#include "backend_definition_binding_internal.h"
#include "backend_runtime_services_internal.h"
#include "lemon/backends/backend_ops.h"
#include "lemon/backends/configured/backend_transport.h"

#include <algorithm>

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
        LaunchServices& services, utils::RequestCancelToken cancel = {}, runtime::EnvironmentSources environment = runtime::environment_sources()) const;
    TransportRequest map_request(BackendOperation operation, OperationDelivery delivery, ModelType model_type,
        const ExecutionSession& session, const json& request, std::optional<std::string_view> original_body = {},
        utils::RequestCancelToken cancel = {}) const;
    TransportRequest map_stream(const std::string& endpoint, ModelType model_type, const ExecutionSession& session,
        const std::string& body, bool sse, long timeout, utils::RequestCancelToken cancel = {}) const;
    json map_response(BackendOperation operation, ModelType model_type, const ExecutionSession& session,
        const json& request, json response, utils::RequestCancelToken cancel = {}) const;
    void resolve_runtime_options(const ModelInfo& model, RecipeOptions& options) const;
    InstallParams install_params(const InstallContext& context, runtime::InstallFactSources sources = runtime::install_fact_sources()) const;
    std::string resolve_version(const std::string& backend, const std::string& file_version, runtime::ProbeSources sources = runtime::probe_sources()) const;
    BackendOps::InstallCheck check_install(const std::string& backend, bool found, runtime::ProbeSources sources = runtime::probe_sources()) const;
    void populate_metadata(ModelInfo& model) const;
    std::string validate_registration(const std::string& checkpoint) const;
    std::string validate_file(const std::string& path) const;
    std::string resolve_artifact(const CheckpointResolveContext& context) const;
    std::string find_imported(const std::string& path) const;
private:
    const execution::OperationProgram& cell(BackendOperation operation, OperationDelivery delivery, ModelType model_type) const;
    runtime::Frame request_frame(const ExecutionSession& session, const json& request) const;
    std::vector<std::size_t> slots(SlotOwnerKind kind, const std::string& owner, const std::string& stage) const;
    std::optional<json> evaluate(const std::string& expression, const runtime::Frame& frame) const;
    void defaults(runtime::Frame& frame, const std::string& stage) const;
    runtime::Frame management_frame(const std::string& backend) const;
    template<class Call>
    auto artifact(const std::string& role, Call call) const {
        const auto found = std::find_if(program_.artifacts.begin(), program_.artifacts.end(), [&](const auto& item) { return item.role == role; });
        if (found == program_.artifacts.end()) throw std::logic_error("No bound artifact consumer");
        const auto schedule = slots(SlotOwnerKind::Artifact, found->name, "artifacts");
        if (schedule.size() != 1) throw std::logic_error("No bound artifact consumer");
        runtime::Frame frame(*executable_);
        frame.facts = {typed::Fact::ModelArtifacts, typed::Fact::ModelLabels};
        return invoke<typed::ArtifactOps>(frame, schedule.front(), SlotOwnerKind::Artifact, found->name, "artifacts", {}, call);
    }
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
