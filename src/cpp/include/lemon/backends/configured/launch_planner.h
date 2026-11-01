#pragma once

#include "lemon/backends/backend_effects.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backends/configured/backend_definition.h"
#include "lemon/backends/configured/container_memory.h"
#include "lemon/backends/container_policy.h"
#include "lemon/model_manager.h"
#include "lemon/utils/container_manager.h"

#include <cstdint>
#include <map>

namespace lemon::backends {

struct LoadContext {
    std::string model_name;
    const ModelInfo& model_info;
    const RecipeOptions& options;
    std::string log_level;
    std::function<void()> backend_selected;
    std::function<void(const std::string& phase, const json& values)> phase_prepared;
    std::function<void(DeviceType)> device_selected;
};
struct ResolvedLaunchInputs {
    json values;
    std::string executable;
    std::vector<std::string> custom_arguments;
    std::vector<std::pair<std::string, std::string>> environment;
};
struct LaunchPlan {
    ServerCommand command;
    ProcessOutput output;
    std::string working_directory;
    long readiness_timeout_seconds;
    bool watchdog_enabled;
    std::optional<utils::ContainerRunSpec> container;
    std::map<std::string, std::string> container_path_rewrites;
};
struct ContainerGpuNode {
    unsigned node;
    std::string architecture;
};
class LaunchServices {
public:
    virtual ~LaunchServices() = default;
    virtual IntegratedMemoryPool integrated_memory_pool() const { throw std::logic_error("Integrated memory pool is unavailable"); }
    virtual FreeMemoryFact free_gpu_memory() const { throw std::logic_error("Free GPU memory is unavailable"); }
    virtual std::optional<ArtifactByteFact> artifact_bytes(const std::string&) const { return {}; }
    virtual std::string container_image(const ContainerPolicy&) const { throw std::logic_error("Container pin service is unavailable"); }
    virtual void install_container_image(const std::string&) { throw std::logic_error("OCI installation service is unavailable"); }
    virtual std::string container_gpu_properties(unsigned) const { throw std::logic_error("Container GPU properties are unavailable"); }
    virtual std::vector<ContainerGpuNode> container_gpu_nodes() const { throw std::logic_error("Container GPU topology is unavailable"); }
    virtual bool container_supports_arch(const std::string&, const std::string&, const std::string&) const { throw std::logic_error("Container GPU support is unavailable"); }
    virtual bool exists(const std::string& path) const = 0;
    virtual std::vector<std::string> supported_backends(const std::string& recipe) const = 0;
    virtual void validate_backend(const std::string& recipe, const std::string& backend) const = 0;
    virtual std::string external_binary(const BackendSpec& spec, const std::string& backend) const = 0;
    virtual void install_backend(const std::string& public_recipe, const std::string& backend) = 0;
    virtual std::string managed_binary(const BackendSpec& spec, const std::string& backend) const = 0;
    virtual int allocate_port() = 0;
    virtual int find_free_port(int start) = 0;
    virtual std::vector<std::pair<std::string, std::string>> environment(
        const std::string& preset, const std::string& executable, const std::string& backend, const std::string& log_category) const = 0;
    virtual PlatformInfo platform() const = 0;
    virtual std::string rocm_channel() const;
    virtual std::string staging_directory(const std::string& program) const;
    virtual long default_timeout_seconds() const = 0;
};
class EffectLaunchServices : public LaunchServices {
public:
    explicit EffectLaunchServices(BackendEffects effects, bool posix_executable_loader_path = false)
        : effects_(std::move(effects)), posix_executable_loader_path_(posix_executable_loader_path) {}
    std::string container_image(const ContainerPolicy& policy) const override { return effects_.container_image(policy); }
    void install_container_image(const std::string& image) override { effects_.install_container_image(image); }
    IntegratedMemoryPool integrated_memory_pool() const override;
    FreeMemoryFact free_gpu_memory() const override;
    std::optional<ArtifactByteFact> artifact_bytes(const std::string& path) const override;
    std::vector<ContainerGpuNode> container_gpu_nodes() const override;
    std::string container_gpu_properties(unsigned node) const override;
    bool container_supports_arch(const std::string& recipe, const std::string& backend, const std::string& arch) const override;
    bool exists(const std::string& path) const override { return effects_.exists(path); }
    std::vector<std::string> supported_backends(const std::string& recipe) const override { return effects_.supported_backends(recipe); }
    void validate_backend(const std::string& recipe, const std::string& backend) const override { effects_.validate_backend(recipe, backend); }
    std::string external_binary(const BackendSpec& spec, const std::string& backend) const override { return effects_.binary.find_external(spec.recipe, backend); }
    void install_backend(const std::string& recipe, const std::string& backend) override { effects_.binary.install(recipe, backend); }
    std::string managed_binary(const BackendSpec& spec, const std::string& backend) const override { return effects_.binary.managed_path(spec, backend); }
    int allocate_port() override { return effects_.allocate_port(); }
    int find_free_port(int start) override { return effects_.find_free_port(start); }
    std::vector<std::pair<std::string, std::string>> environment(const std::string& preset, const std::string& executable,
        const std::string& backend, const std::string& log_category) const override;
    PlatformInfo platform() const override { return native_platform(); }
    long default_timeout_seconds() const override;
private:
    BackendEffects effects_;
    bool posix_executable_loader_path_;
};
class LaunchPlanner {
public:
    void require_platform(const BackendDefinition& definition, const LaunchServices& services) const;
    ResolvedLaunchInputs prepare(const BackendDefinition& definition, const BackendSpec& spec,
                                const LoadContext& context, LaunchServices& services) const;
    LaunchPlan plan(const BackendDefinition& definition, const ResolvedLaunchInputs& inputs) const;
};

}
