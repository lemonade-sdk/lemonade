#pragma once

#include "lemon/backends/backend_effects.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backends/configured/backend_definition.h"
#include "lemon/model_manager.h"

namespace lemon::backends {

struct LoadContext {
    std::string model_name;
    const ModelInfo& model_info;
    const RecipeOptions& options;
    std::string log_level;
    bool do_not_upgrade;
    std::function<void()> backend_selected;
    std::function<void(const std::string& phase, const json& values)> phase_prepared;
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
};
class LaunchServices {
public:
    virtual ~LaunchServices() = default;
    virtual bool exists(const std::string& path) const = 0;
    virtual std::vector<std::string> supported_backends(const std::string& recipe) const = 0;
    virtual void validate_backend(const std::string& recipe, const std::string& backend) const = 0;
    virtual std::string external_binary(const BackendSpec& spec, const std::string& backend) const = 0;
    virtual void install_backend(const std::string& public_recipe, const std::string& backend) = 0;
    virtual std::string managed_binary(const BackendSpec& spec, const std::string& backend) const = 0;
    virtual int allocate_port() = 0;
    virtual std::vector<std::pair<std::string, std::string>> environment(
        const std::string& preset, const std::string& executable, const std::string& backend, const std::string& log_category) const = 0;
    virtual PlatformInfo platform() const = 0;
    virtual long default_timeout_seconds() const = 0;
};
class EffectLaunchServices : public LaunchServices {
public:
    explicit EffectLaunchServices(BackendEffects effects, bool posix_executable_loader_path = false)
        : effects_(std::move(effects)), posix_executable_loader_path_(posix_executable_loader_path) {}
    bool exists(const std::string& path) const override { return effects_.exists(path); }
    std::vector<std::string> supported_backends(const std::string& recipe) const override { return effects_.supported_backends(recipe); }
    void validate_backend(const std::string& recipe, const std::string& backend) const override { effects_.validate_backend(recipe, backend); }
    std::string external_binary(const BackendSpec& spec, const std::string& backend) const override { return effects_.binary.find_external(spec.recipe, backend); }
    void install_backend(const std::string& recipe, const std::string& backend) override { effects_.binary.install(recipe, backend); }
    std::string managed_binary(const BackendSpec& spec, const std::string& backend) const override { return effects_.binary.managed_path(spec, backend); }
    int allocate_port() override { return effects_.allocate_port(); }
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
    ResolvedLaunchInputs prepare(const BackendDefinition& definition, const BackendSpec& spec,
                                const LoadContext& context, LaunchServices& services) const;
    LaunchPlan plan(const BackendDefinition& definition, const ResolvedLaunchInputs& inputs) const;
};

}
