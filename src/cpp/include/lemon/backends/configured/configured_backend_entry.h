#pragma once

#include "lemon/backends/backend_registry.h"
#include "lemon/backends/configured/configured_backend_ops.h"
#include "lemon/server_process.h"

namespace lemon::backends {

class DefinitionRunner;
class ExecutableDefinition;
enum class TrustedCompatibility { None, LegacyNonChatSse };

struct BuiltinLoadLog {
    std::function<void(const std::string& category, const std::string& model_name, const RecipeOptions& options)> loading;
    std::function<void(const std::string& category, const std::string& phase, const json& values)> prepared;
    std::function<void(const std::string& category, const json& values, const ServerCommand& command)> starting;
};

struct BuiltinLaunchCompatibility {
    std::optional<DeviceType> device_on_backend_selection;
    bool posix_executable_loader_path = false;
    std::optional<BuiltinLoadLog> load_log;
};

std::vector<OperationBinding> configured_operation_bindings(const BackendDefinition& definition, TrustedCompatibility trusted);

struct ConfiguredBackendEntry {
private:
    struct FactoryState;
    struct V2FactoryState;
    const std::shared_ptr<FactoryState> factory_state_;
    const std::shared_ptr<V2FactoryState> v2_factory_state_;
    explicit ConfiguredBackendEntry(std::shared_ptr<const ExecutableDefinition> executable);
    const std::shared_ptr<const DefinitionRunner> runner_;
    friend class ConfiguredServer;
public:
    static std::shared_ptr<const ConfiguredBackendEntry> make(std::shared_ptr<const ExecutableDefinition> executable);
    static std::shared_ptr<const ConfiguredBackendEntry> make(std::shared_ptr<const BackendDefinition> definition, TrustedCompatibility compatibility = TrustedCompatibility::None, BuiltinLaunchCompatibility launch_compatibility = {});
    explicit ConfiguredBackendEntry(std::shared_ptr<const BackendDefinition> definition, TrustedCompatibility compatibility = TrustedCompatibility::None, BuiltinLaunchCompatibility launch_compatibility = {});
    ConfiguredBackendEntry(const ConfiguredBackendEntry&) = delete;
    ConfiguredBackendEntry& operator=(const ConfiguredBackendEntry&) = delete;
    const std::shared_ptr<const BackendDefinition> definition;
    const BackendDescriptor descriptor;
    const std::vector<OperationBinding> bindings;
    const std::shared_ptr<const ConfiguredBackendOps> ops;
    const std::shared_ptr<const BackendOps> v2_ops;
    const std::shared_ptr<const ExecutableDefinition> executable_definition;
    const BackendOps* backend_ops() const { return v2_ops ? v2_ops.get() : ops.get(); }
    const json& records() const;
    const BackendSpec spec;
    const TrustedCompatibility compatibility;
    const BuiltinLaunchCompatibility launch_compatibility;
    const BackendCreateFn factory;
};

}
