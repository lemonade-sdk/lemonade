#pragma once

#include "backend_definition_execution_internal.h"
#include "backend_hook_types.h"
#include "lemon/backends/configured/launch_planner.h"
#include "lemon/utils/http_client.h"
#include "lemon/utils/process_manager.h"

#include <atomic>
#include <map>
#include <memory>
#include <set>
#include <thread>

namespace lemon::backends {

class DefinitionRunner;
class ExecutableDefinition;

namespace runtime {

struct Frame {
    execution::InvocationFrame data;
    std::set<typed::Fact> facts;
    std::set<std::size_t> produced;
    std::set<std::string> explicit_options;
    const ExecutableDefinition* executable;
    const std::uint64_t nonce;
    std::optional<std::size_t> active_slot;
    explicit Frame(const ExecutableDefinition& owner);
    Frame(Frame&&) = default;
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;
};

class InvocationGuard final {
public:
    ~InvocationGuard();
    InvocationGuard(const InvocationGuard&) = delete;
    InvocationGuard& operator=(const InvocationGuard&) = delete;
    void check(typed::Effect effect, std::size_t allowance = 1);
    void check_active() const;
    std::weak_ptr<const std::atomic<bool>> lifetime() const { return lifetime_; }
private:
    InvocationGuard(Frame& frame, const ExecutableDefinition& executable, std::size_t slot,
        const execution::SymbolicHook& hook, SlotOwnerKind kind, const std::string& owner,
        const std::string& stage, utils::RequestCancelToken cancel);
    Frame& frame_;
    const ExecutableDefinition* executable_;
    const std::size_t slot_;
    const LogicalHookSlot owner_;
    const std::uint64_t nonce_;
    const std::vector<typed::Effect> effects_;
    const std::thread::id thread_;
    const utils::RequestCancelToken cancel_;
    std::size_t remaining_ = 1024 * 1024;
    bool active_ = true;
    const std::shared_ptr<std::atomic<bool>> lifetime_ = std::make_shared<std::atomic<bool>>(true);
    friend class ::lemon::backends::DefinitionRunner;
#ifdef LEMONADE_RUNTIME_TEST
    friend struct TestAccess;
#endif
};

void check_cancelled(const utils::RequestCancelToken& cancel);
bool invocation_active(const std::weak_ptr<const std::atomic<bool>>& lifetime);
typed::Platform typed_platform(const std::string& os);

struct InstallFactSources {
    std::function<std::string()> therock_version;
    std::function<std::string()> rocm_asset_family;
    std::function<std::string()> cuda_architecture;
    std::function<std::string()> rocm_channel = [] { return std::string("stable"); };
};
InstallFactSources install_fact_sources();

class InstallServices final : public typed::InstallFactsServices {
public:
    InstallServices(InvocationGuard& guard, typed::InstallFactsInput input, InstallFactSources sources);
    std::string therock_version() override;
    std::string rocm_asset_family() override;
    std::string cuda_architecture() override;
private:
    std::string read(typed::InstallFact fact, const std::string& backend, const std::function<std::string()>& source);
    InvocationGuard& guard_;
    const typed::InstallFactsInput input_;
    const InstallFactSources sources_;
};

struct ProbeSources {
    std::function<utils::CapturedProcess(std::size_t, unsigned, const std::function<bool()>&)> version;
    std::function<bool()> kfd;
    std::function<bool()> hip_plugin;
};
ProbeSources probe_sources();
class SystemServices final : public typed::SystemProbeServices {
public:
    SystemServices(InvocationGuard& guard, typed::SystemInstallInput input, ProbeSources sources);
    std::string llama_version() override;
    bool kfd_present() override;
    bool hip_plugin_present() override;
private:
    void check(bool linux_only = false);
    InvocationGuard& guard_;
    const typed::SystemInstallInput input_;
    const ProbeSources sources_;
};

struct EnvironmentSources {
    std::function<std::optional<std::string>(typed::ParentKey)> parent;
    std::function<std::string(typed::RuntimeQuery)> runtime;
    std::function<void(const std::string&, const std::string&)> stage;
};
EnvironmentSources environment_sources();
class EnvironmentServices;
class StagingPlan final {
public:
    StagingPlan(const StagingPlan&) = delete;
    StagingPlan(StagingPlan&&) = default;
private:
    StagingPlan(InvocationGuard& guard, typed::EnvironmentPlan plan, std::string destination,
        std::function<void(const std::string&, const std::string&)> stage)
        : guard_(&guard), lifetime_(guard.lifetime()), plan_(std::move(plan)), destination_(std::move(destination)), stage_(std::move(stage)) {}
    InvocationGuard* guard_;
    std::weak_ptr<const std::atomic<bool>> lifetime_;
    typed::EnvironmentPlan plan_;
    std::string destination_;
    std::function<void(const std::string&, const std::string&)> stage_;
    friend class EnvironmentServices;
    friend class StagingConsumer;
#ifdef LEMONADE_RUNTIME_TEST
    friend struct TestAccess;
#endif
};
class StagingConsumer final {
public:
    static std::vector<std::pair<std::string, std::string>> execute(StagingPlan plan);
};
class EnvironmentServices final : public typed::EnvironmentServices {
public:
    std::optional<std::string> parent(typed::ParentKey key) override;
    std::string runtime(typed::RuntimeQuery query) override;
private:
    EnvironmentServices(InvocationGuard& guard, EnvironmentSources sources, std::string destination, std::optional<typed::StagingScope> scope);
    std::optional<typed::StagingCapability> capability();
    StagingPlan seal(typed::EnvironmentPlan plan);
    InvocationGuard& guard_;
    const EnvironmentSources sources_;
    const std::string destination_;
    const std::optional<typed::StagingScope> scope_;
    std::map<typed::ParentKey, std::optional<std::string>> parents_;
    std::map<typed::RuntimeQuery, std::string> runtime_;
    std::set<std::string> architectures_;
    friend class ::lemon::backends::DefinitionRunner;
#ifdef LEMONADE_RUNTIME_TEST
    friend struct TestAccess;
#endif
};

class ArtifactServices final : public typed::ArtifactReadServices {
public:
    ArtifactServices(InvocationGuard& guard, const std::string& approved_path, bool directory);
    ~ArtifactServices();
    typed::ArtifactHandle root() const;
    std::optional<typed::ArtifactHeader> header(typed::ArtifactHandle file) override;
    std::string prefix(typed::ArtifactHandle file, std::size_t bytes) override;
    std::vector<typed::ArtifactEntry> entries(typed::ArtifactHandle root) override;
    std::string approved_result(const std::string& path);
private:
    struct Inventory;
    InvocationGuard& guard_;
    const std::weak_ptr<const std::atomic<bool>> lifetime_;
    const std::unique_ptr<Inventory> inventory_;
    void check(std::size_t bytes = 1);
    std::size_t lookup(typed::ArtifactHandle file);
};

class SelectionServices final : public typed::BackendSelectionServices {
public:
    SelectionServices(InvocationGuard& guard, std::vector<std::string> supported);
    bool supports(const std::string& backend) override;
    typed::DeviceCheck validate_device(const typed::DeviceValidationInput& input) override;
private:
    InvocationGuard& guard_;
    const std::vector<std::string> supported_;
};

}
}
